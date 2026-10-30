// Included inside nr::pe after the native copy and keyed-mutex helpers.
bool Session::record_raw_bridge(IUnknown* device, ID3D12CommandQueue* queue,
                                const std::function<void()>& flush,
                                const std::vector<BridgeSurface>& surfaces, const RawRecorder11& recorder) {
    auto& s = *impl_;
    if (!device || surfaces.empty() || !recorder || s.failed) return false;
    ID3D12Device* d12 = nullptr;
    ID3D11Device* d11 = nullptr;
    ID3D10Device* d10 = nullptr;
    ID3D11DeviceContext* context = nullptr;
    std::vector<IUnknown*> held;
    auto release = on_exit([&] {
        for (IUnknown* p : held) p->Release();
        if (context) context->Release();
        if (d12) d12->Release();
        if (d11) d11->Release();
        if (d10) d10->Release();
    });
    device->QueryInterface(IID_PPV_ARGS(&d12));
    if (!d12) device->QueryInterface(IID_PPV_ARGS(&d11));
    if (!d12 && !d11) device->QueryInterface(IID_PPV_ARGS(&d10));
    if ((!d12 && !d11 && !d10) || (d12 && !queue)) return false;
    if (!s.decide_mode(false) || !s.ensure_bridge(d12 ? d12->GetAdapterLuid() : adapter_luid(device))) return false;
    if (d10 && !s.bdev->keyed_mutex()) return false;
    if (d12) {
        if (!s.own_queue) { s.own_queue = queue; queue->AddRef(); }
        if (s.own_queue != queue) return false;
    }
    if (flush) flush();
    if (d11) d11->GetImmediateContext(&context);
    ++s.raw_calls;
    ++s.bridge_runs;
    s.sweep_retired_images();
    // Old ReShade textures may have been destroyed after a resize. Their shared twins are ours; retire
    // them only once Vulkan has stopped using them, then let the bridge retain the D3D side to its fence.
    for (auto it = s.raw_shared.begin(); it != s.raw_shared.end();) {
        if (s.raw_calls - it->second.used > 64 && !s.bdev->busy(it->second.image.image)) {
            s.bdev->bury(it->second.image);
            it = s.raw_shared.erase(it);
        } else ++it;
    }
    struct Item { IUnknown* key; IUnknown* source; bridge::Image* shared; D3D12_RESOURCE_STATES state; bool write; };
    std::vector<Item> items;
    for (const BridgeSurface& surface : surfaces) {
        if (!surface.resource) continue;
        auto found = std::find_if(items.begin(), items.end(), [&](const Item& i) { return i.key == surface.resource; });
        if (found != items.end()) {
            if (found->state != surface.state) return false;
            found->write |= surface.write;
            continue;
        }
        uint32_t w = 0, h = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        IUnknown* source = nullptr;
        if (d12) {
            ID3D12Resource* r = nullptr;
            if (FAILED(surface.resource->QueryInterface(IID_PPV_ARGS(&r)))) return false;
            source = r; held.push_back(r);
            const auto desc = r->GetDesc();
            if (!plain_2d(desc) || desc.Width > UINT32_MAX) return false;
            w = uint32_t(desc.Width); h = desc.Height; format = desc.Format;
        } else if (d11) {
            ID3D11Texture2D* r = nullptr;
            if (FAILED(surface.resource->QueryInterface(IID_PPV_ARGS(&r)))) return false;
            source = r; held.push_back(r);
            D3D11_TEXTURE2D_DESC desc{}; r->GetDesc(&desc);
            if (desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) return false;
            w = desc.Width; h = desc.Height; format = desc.Format;
        } else {
            ID3D10Texture2D* r = nullptr;
            if (FAILED(surface.resource->QueryInterface(IID_PPV_ARGS(&r)))) return false;
            source = r; held.push_back(r);
            D3D10_TEXTURE2D_DESC desc{}; r->GetDesc(&desc);
            if (desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) return false;
            w = desc.Width; h = desc.Height; format = desc.Format;
        }
        format = bridge::shared_format(format);
        if (format == DXGI_FORMAT_UNKNOWN) return false;
        auto& shared = s.raw_shared[surface.resource];
        shared.used = s.raw_calls;
        if (!s.ensure_shared(shared.image, w, h, format, d12, d11, "ReShade auxiliary pass", d10)) return false;
        if (surface.write && !shared.image.storage) return false;
        items.push_back({surface.resource, source, &shared.image, surface.state, surface.write});
    }
    if (items.empty()) return false;
    std::vector<const bridge::Image*> images;
    for (const Item& i : items) images.push_back(i.shared);
    std::string why;
    bridge::Job* job = s.bdev->begin(&why, &s.raw_shared);
    if (!job) { s.status = why; return false; }
    const uint64_t generation = s.bdev->generation(job);
    auto ticket = std::make_shared<nr::gpu::Recording>();
    ticket->submitted([d = s.bdev, job, generation] { return !d->busy(job, generation); });
    ticket->seal();
    nr::gpu::Scope scope(ticket);
    s.bdev->acquire(job, images);
    const RawLookup11 lookup = [&](IUnknown* key) {
        RawImage out;
        const auto i = std::find_if(items.begin(), items.end(), [&](const Item& i) { return i.key == key; });
        if (i != items.end()) {
            out.image = i->shared->image; out.format = i->shared->format;
            out.width = i->shared->width; out.height = i->shared->height;
            out.layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        return out;
    };
    bool recorded = false;
    try { recorded = recorder(s.bdev->command_buffer(job), s.make_host(), lookup); }
    catch (const std::exception& e) { s.status = e.what(); log("[nr] auxiliary pass: %s", e.what()); }
    if (!recorded) { s.bdev->abort(job); return false; }
    if (!s.bdev->end(job, &why)) { s.status = why; return false; }
    auto discard = on_exit([&] { s.bdev->discard(job, generation); });
    if (d12) {
        auto* in = s.next_own_list(d12);
        auto* out = s.next_own_list(d12);
        const auto give_back = [&](ID3D12GraphicsCommandList* list) {
            if (!list) return;
            list->Close();
            for (auto& slot : s.own_lists) if (slot.list == list) slot.done = 0;
        };
        if (!in || !out) { give_back(in); give_back(out); return false; }
        const auto transition = [](ID3D12GraphicsCommandList* list, ID3D12Resource* r,
                                   D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            if (before == after) return;
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
            list->ResourceBarrier(1, &b);
        };
        for (const Item& item : items) {
            auto* source = static_cast<ID3D12Resource*>(item.source);
            auto* shared = static_cast<ID3D12Resource*>(item.shared->d3d);
            transition(in, source, item.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(in, shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            in->CopyResource(shared, source);
            transition(in, shared, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            transition(in, source, D3D12_RESOURCE_STATE_COPY_SOURCE, item.state);
            if (item.write) {
                transition(out, source, item.state, D3D12_RESOURCE_STATE_COPY_DEST);
                transition(out, shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                out->CopyResource(source, shared);
                transition(out, shared, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                transition(out, source, D3D12_RESOURCE_STATE_COPY_DEST, item.state);
            }
        }
        if (FAILED(in->Close()) || FAILED(out->Close())) { give_back(in); give_back(out); return false; }
        ID3D12CommandList* first = in;
        queue->ExecuteCommandLists(1, &first);
        const bool ran = s.bdev->run_between(queue, job, generation);
        if (ran) { ID3D12CommandList* last = out; queue->ExecuteCommandLists(1, &last); }
        if (FAILED(queue->Signal(s.own_lists_fence, s.own_lists_value + 1))) {
            for (auto& slot : s.own_lists) if (slot.list == in || slot.list == out) slot.done = ~0ull - 1;
            s.bdev->lost("the auxiliary pass completion signal failed");
            return false;
        }
        ++s.own_lists_value;
        for (auto& slot : s.own_lists)
            if (slot.list == in || slot.list == out) slot.done = ran || slot.list == in ? s.own_lists_value : 0;
        return ran;
    }
    if (d11) {
        ID3D11Predicate* predicate = nullptr; BOOL value = FALSE;
        context->GetPredication(&predicate, &value);
        if (predicate) context->SetPredication(nullptr, FALSE);
        auto restore = on_exit([&] { if (predicate) { context->SetPredication(predicate, value); predicate->Release(); } });
        for (const Item& item : items)
            context->CopyResource(static_cast<ID3D11Resource*>(item.shared->d3d), static_cast<ID3D11Resource*>(item.source));
        if (!s.bdev->run_between(context, job, generation)) return false;
        for (const Item& item : items) if (item.write)
            context->CopyResource(static_cast<ID3D11Resource*>(item.source), static_cast<ID3D11Resource*>(item.shared->d3d));
        return true;
    }
    const Keyed acquired = acquire_all(images, 0, 1000);
    if (acquired != Keyed::Ok) return false;
    ID3D10Predicate* predicate = nullptr; BOOL value = FALSE;
    d10->GetPredication(&predicate, &value);
    if (predicate) d10->SetPredication(nullptr, FALSE);
    auto restore = on_exit([&] { if (predicate) { d10->SetPredication(predicate, value); predicate->Release(); } });
    for (const Item& item : items)
        d10->CopyResource(static_cast<ID3D10Resource*>(item.shared->d3d), static_cast<ID3D10Resource*>(item.source));
    std::vector<bool> released;
    if (!release_all(images, 1, &released)) {
        std::vector<const bridge::Image*> back;
        for (size_t i = 0; i < images.size(); ++i) if (released[i]) back.push_back(images[i]);
        if (acquire_all(back, 1, 1000) != Keyed::Ok || !release_all(images, 0)) s.failed = true;
        return false;
    }
    d10->Flush();
    if (!s.bdev->submit_keyed(job, generation, images, 1, 2)) {
        if (acquire_all(images, 1, 1000) != Keyed::Ok || !release_all(images, 0)) s.failed = true;
        return false;
    }
    if (acquire_all(images, 2, 1000) != Keyed::Ok) { s.failed = true; return false; }
    for (const Item& item : items) if (item.write)
        d10->CopyResource(static_cast<ID3D10Resource*>(item.source), static_cast<ID3D10Resource*>(item.shared->d3d));
    if (!release_all(images, 0)) s.failed = true;
    return !s.failed;
}
