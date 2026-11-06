#include "session_manager.hpp"

#include <libtorrent/session_params.hpp>

namespace bt {

SessionManager::SessionManager() {
    libtorrent::session_params params;
    params.settings.set_int(
        libtorrent::settings_pack::alert_mask,
        libtorrent::alert_category::error |
        libtorrent::alert_category::status |
        libtorrent::alert_category::peer |
        libtorrent::alert_category::tracker);

    // Cap upload (seed) rate at 100 KB/s.
    params.settings.set_int(
        libtorrent::settings_pack::upload_rate_limit, 800 * 1024);

    session_ = std::make_unique<libtorrent::session>(std::move(params));
}

SessionManager::~SessionManager() {
    session_->pause();
    session_.reset();
}

libtorrent::session_handle& SessionManager::session() {
    return *session_;
}

const libtorrent::session_handle& SessionManager::session() const {
    return *session_;
}

}  // namespace bt
