#include "session_manager.hpp"

#include <libtorrent/fingerprint.hpp>
#include <libtorrent/posix_disk_io.hpp>
#include <libtorrent/session_params.hpp>

namespace bt {

SessionManager::SessionManager() {
    libtorrent::session_params params;
    params.disk_io_constructor = libtorrent::posix_disk_io_constructor;
    params.settings.set_str(
        libtorrent::settings_pack::user_agent, "Swarmbit/1.0.0");
    params.settings.set_str(
        libtorrent::settings_pack::handshake_client_version, "Swarmbit/1.0.0");
    params.settings.set_str(
        libtorrent::settings_pack::peer_fingerprint,
        libtorrent::generate_fingerprint("SB", 1, 0, 0));
    params.settings.set_int(
        libtorrent::settings_pack::alert_mask,
        libtorrent::alert_category::error |
        libtorrent::alert_category::status |
        libtorrent::alert_category::peer |
        libtorrent::alert_category::tracker);

    // Cap upload (seed) rate at 100 KB/s.
    params.settings.set_int(
        libtorrent::settings_pack::upload_rate_limit, 800 * 1024);
    params.settings.set_int(
        libtorrent::settings_pack::connections_limit, 200);
    params.settings.set_int(
        libtorrent::settings_pack::initial_picker_threshold, 0);
    params.settings.set_bool(
        libtorrent::settings_pack::auto_sequential, true);
    params.settings.set_bool(
        libtorrent::settings_pack::piece_extent_affinity, false);
    params.settings.set_bool(
        libtorrent::settings_pack::close_redundant_connections, false);

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
