// SPDX-License-Identifier: Apache-2.0
#include "app/documentloader.h"

#include "app/platform.h"

#include <utility>

namespace rgui {

DocumentLoader::DocumentLoader(Platform& platform, Callback onLoaded)
    : platform_(platform), callbacks_(std::make_shared<Callbacks>()) {
    callbacks_->onLoaded = std::move(onLoaded);
    thread_ = std::thread([this] { Run(); });
}

DocumentLoader::~DocumentLoader() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        pending_.reset();
    }
    wake_.notify_all();
    thread_.join();
    callbacks_->alive = false;
}

uint64_t DocumentLoader::Load(std::string path) {
    Request request;
    request.id = ++nextId_;
    request.path = std::move(path);
    const uint64_t id = request.id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(request);
    }
    wake_.notify_one();
    return id;
}

void DocumentLoader::Run() {
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || pending_.has_value(); });
            if (stop_) return;
            request = std::move(*pending_);
            pending_.reset();
        }
        DocumentLoaded loaded;
        loaded.id = request.id;
        loaded.ok = loaded.document.Open(request.path, loaded.status);
        if (loaded.ok) loaded.stats = loaded.document.Stats();
        loaded.path = std::move(request.path);
        platform_.PostToUi([callbacks = callbacks_, loaded = std::move(loaded)]() mutable {
            if (callbacks->alive && callbacks->onLoaded) callbacks->onLoaded(std::move(loaded));
        });
    }
}

} // namespace rgui
