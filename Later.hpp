// Later::run(): Hyprland's doLater (the next event loop iteration), cancelled
// by Later::cancelAll() when the plugin unloads, so no callback runs code
// that is gone. Every deferred call in hyprgrid goes through here.
#pragma once

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>

namespace Later {
    inline std::set<uint64_t>& pending() {
        static std::set<uint64_t> seqs;
        return seqs;
    }

    inline void run(std::function<void()> fn) {
        // The id is known only once doLater returns: the callback reads it
        // from a shared slot filled right after.
        auto       seq = std::make_shared<uint64_t>(0);
        const auto ID  = g_pEventLoopManager->doLater([seq, fn = std::move(fn)] {
            pending().erase(*seq);
            fn();
        });
        *seq           = ID;
        pending().insert(ID);
    }

    inline void cancelAll() {
        for (const auto SEQ : pending())
            g_pEventLoopManager->removeDoLater(SEQ);
        pending().clear();
    }
}
