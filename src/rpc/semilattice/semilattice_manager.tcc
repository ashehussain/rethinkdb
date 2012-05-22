#include "rpc/semilattice/semilattice_manager.hpp"

#include "errors.hpp"
#include <boost/make_shared.hpp>

#include "concurrency/cross_thread_signal.hpp"
#include "concurrency/pmap.hpp"
#include "concurrency/wait_any.hpp"
#include "logger.hpp"

template<class metadata_t>
semilattice_manager_t<metadata_t>::semilattice_manager_t(message_service_t *ms, const metadata_t &initial_metadata) :
    message_service(ms),
    root_view(boost::make_shared<root_view_t>(this)),
    metadata_version(0),
    metadata(initial_metadata),
    next_sync_from_query_id(0), next_sync_to_query_id(0),
    event_watcher(
        boost::bind(&semilattice_manager_t<metadata_t>::on_connect, this, _1),
        boost::bind(&semilattice_manager_t<metadata_t>::on_disconnect, this, _1))
{
    ASSERT_FINITE_CORO_WAITING;
    connectivity_service_t::peers_list_freeze_t freeze(message_service->get_connectivity_service());
    rassert(message_service->get_connectivity_service()->get_peers_list().empty());
    event_watcher.reset(message_service->get_connectivity_service(), &freeze);
}

template<class metadata_t>
semilattice_manager_t<metadata_t>::~semilattice_manager_t() THROWS_NOTHING {
    assert_thread();
    root_view->parent = NULL;
}

template<class metadata_t>
boost::shared_ptr<semilattice_readwrite_view_t<metadata_t> > semilattice_manager_t<metadata_t>::get_root_view() {
    assert_thread();
    return root_view;
}

template<class metadata_t>
semilattice_manager_t<metadata_t>::root_view_t::root_view_t(semilattice_manager_t *p)
    : parent(p) {
    parent->assert_thread();
}

template<class metadata_t>
metadata_t semilattice_manager_t<metadata_t>::root_view_t::get() {
    rassert(parent, "accessing `semilattice_manager_t` root view when cluster no longer exists");
    parent->assert_thread();
    return parent->metadata;
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::root_view_t::join(const metadata_t &added_metadata) {
    rassert(parent, "accessing `semilattice_manager_t` root view when cluster no longer exists");
    parent->assert_thread();

    metadata_version_t new_version = ++parent->metadata_version;
    parent->join_metadata_locally(added_metadata);

    /* Distribute changes to all peers we can currently see. If we can't
    currently see a peer, that's OK; it will hear about the metadata change when
    it reconnects, via the `semilattice_manager_t`'s `on_connect()` handler. */
    connectivity_service_t::peers_list_freeze_t freeze(parent->message_service->get_connectivity_service());
    std::set<peer_id_t> peers = parent->message_service->get_connectivity_service()->get_peers_list();
    for (std::set<peer_id_t>::iterator it = peers.begin(); it != peers.end(); it++) {
        if (*it != parent->message_service->get_connectivity_service()->get_me()) {
            coro_t::spawn_sometime(boost::bind(
                &semilattice_manager_t<metadata_t>::send_metadata_to_peer, parent,
                *it, added_metadata, new_version,
                auto_drainer_t::lock_t(parent->drainers.get())
                ));
        }
    }
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::root_view_t::sync_from(peer_id_t peer, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t, sync_failed_exc_t) {
    rassert(parent, "accessing `semilattice_manager_t` root view when cluster no longer exists");
    parent->assert_thread();
    sync_from_query_id_t query_id = parent->next_sync_from_query_id++;
    promise_t<metadata_version_t> response_cond;
    map_insertion_sentry_t<sync_from_query_id_t, promise_t<metadata_version_t> *> response_listener(&parent->sync_from_waiters, query_id, &response_cond);
    disconnect_watcher_t watcher(parent->message_service->get_connectivity_service(), peer);
    parent->message_service->send_message(peer,
        boost::bind(&semilattice_manager_t<metadata_t>::write_sync_from_query, _1, query_id));
    wait_any_t waiter(response_cond.get_ready_signal(), &watcher);
    wait_interruptible(&waiter, interruptor);   /* May throw `interrupted_exc_t` */
    if (watcher.is_pulsed()) {
        throw sync_failed_exc_t();
    }
    parent->wait_for_version_from_peer(peer, response_cond.get_value(), interruptor);
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::root_view_t::sync_to(peer_id_t peer, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t, sync_failed_exc_t) {
    rassert(parent, "accessing `semilattice_manager_t` root view when cluster no longer exists");
    parent->assert_thread();
    sync_to_query_id_t query_id = parent->next_sync_to_query_id++;
    cond_t response_cond;
    map_insertion_sentry_t<sync_to_query_id_t, cond_t *> response_listener(&parent->sync_to_waiters, query_id, &response_cond);
    disconnect_watcher_t watcher(parent->message_service->get_connectivity_service(), peer);
    parent->message_service->send_message(peer,
        boost::bind(&semilattice_manager_t<metadata_t>::write_sync_to_query, _1, query_id, parent->metadata_version));
    wait_any_t waiter(&response_cond, &watcher);
    wait_interruptible(&waiter, interruptor);   /* May throw `interrupted_exc_t` */
    if (watcher.is_pulsed()) {
        throw sync_failed_exc_t();
    }
    rassert(response_cond.is_pulsed());
}

template<class metadata_t>
publisher_t<boost::function<void()> > *semilattice_manager_t<metadata_t>::root_view_t::get_publisher() {
    rassert(parent, "accessing `semilattice_manager_t` root view when cluster no longer exists");
    parent->assert_thread();
    return parent->metadata_publisher.get_publisher();
}

static const char message_code_metadata = 'M';
static const char message_code_sync_from_query = 'F';
static const char message_code_sync_from_reply = 'f';
static const char message_code_sync_to_query = 'T';
static const char message_code_sync_to_reply = 't';

template<class metadata_t>
void semilattice_manager_t<metadata_t>::write_metadata(write_stream_t *stream, metadata_t md, metadata_version_t mdv) {
    write_message_t msg;
    uint8_t code = message_code_metadata;
    msg << code;
    msg << md;
    msg << mdv;
    int res = send_write_message(stream, &msg);
    if (res) { throw fake_archive_exc_t(); }
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::write_sync_from_query(write_stream_t *stream, sync_from_query_id_t query_id) {
    write_message_t msg;
    uint8_t code = message_code_sync_from_query;
    msg << code;
    msg << query_id;
    int res = send_write_message(stream, &msg);
    if (res) { throw fake_archive_exc_t(); }
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::write_sync_from_reply(write_stream_t *stream, sync_from_query_id_t query_id, metadata_version_t version) {
    write_message_t msg;
    uint8_t code = message_code_sync_from_reply;
    msg << code;
    msg << query_id;
    msg << version;
    int res = send_write_message(stream, &msg);
    if (res) { throw fake_archive_exc_t(); }
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::write_sync_to_query(write_stream_t *stream, sync_to_query_id_t query_id, metadata_version_t version) {
    write_message_t msg;
    uint8_t code = message_code_sync_to_query;
    msg << code;
    msg << query_id;
    msg << version;
    int res = send_write_message(stream, &msg);
    if (res) { throw fake_archive_exc_t(); }
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::write_sync_to_reply(write_stream_t *stream, sync_to_query_id_t query_id) {
    write_message_t msg;
    uint8_t code = message_code_sync_to_reply;
    msg << code;
    msg << query_id;
    int res = send_write_message(stream, &msg);
    if (res) { throw fake_archive_exc_t(); }
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::on_message(peer_id_t sender, read_stream_t *stream) {
    uint8_t code;
    {
        int res = deserialize(stream, &code);
        if (res) { throw fake_archive_exc_t(); }
    }

    switch (code) {
        case message_code_metadata: {
            metadata_t added_metadata;
            metadata_version_t change_version;
            {
                int res = deserialize(stream, &added_metadata);
                if (res) { throw fake_archive_exc_t(); }
                res = deserialize(stream, &change_version);
                if (res) { throw fake_archive_exc_t(); }
            }
            coro_t::spawn_sometime(boost::bind(
                &semilattice_manager_t<metadata_t>::deliver_metadata_on_home_thread, this,
                sender, added_metadata, change_version, auto_drainer_t::lock_t(drainers.get())
                ));
            break;
        }
        case message_code_sync_from_query: {
            sync_from_query_id_t query_id;
            {
                int res = deserialize(stream, &query_id);
                if (res) { throw fake_archive_exc_t(); }
            }
            coro_t::spawn_sometime(boost::bind(
                &semilattice_manager_t<metadata_t>::deliver_sync_from_query_on_home_thread, this,
                sender, query_id, auto_drainer_t::lock_t(drainers.get())
                ));
            break;
        }
        case message_code_sync_from_reply: {
            sync_from_query_id_t query_id;
            metadata_version_t version;
            {
                int res = deserialize(stream, &query_id);
                if (res) { throw fake_archive_exc_t(); }
                res = deserialize(stream, &version);
                if (res) { throw fake_archive_exc_t(); }
            }
            coro_t::spawn_sometime(boost::bind(
                &semilattice_manager_t<metadata_t>::deliver_sync_from_reply_on_home_thread, this,
                sender, query_id, version, auto_drainer_t::lock_t(drainers.get())
                ));
            break;
        }
        case message_code_sync_to_query: {
            sync_from_query_id_t query_id;
            metadata_version_t version;
            {
                int res = deserialize(stream, &query_id);
                if (res) { throw fake_archive_exc_t(); }
                res = deserialize(stream, &version);
                if (res) { throw fake_archive_exc_t(); }
            }
            coro_t::spawn_sometime(boost::bind(
                &semilattice_manager_t<metadata_t>::deliver_sync_to_query_on_home_thread, this,
                sender, query_id, version, auto_drainer_t::lock_t(drainers.get())
                ));
            break;
        }
        case message_code_sync_to_reply: {
            sync_from_query_id_t query_id;
            {
                int res = deserialize(stream, &query_id);
                if (res) { throw fake_archive_exc_t(); }
            }
            coro_t::spawn_sometime(boost::bind(
                &semilattice_manager_t<metadata_t>::deliver_sync_to_reply_on_home_thread, this,
                sender, query_id, auto_drainer_t::lock_t(drainers.get())
                ));
            break;
        }
        default: {
            /* We don't try to tolerate garbage on the wire. The network layer had
            better not corrupt our messages. If we need to, we'll add more
            checksums. */
            crash("Unexpected semilattice message code: %d", (int)code);
        }
    }
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::on_connect(peer_id_t peer) {
    assert_thread();

    /* We have to spawn this in a separate coroutine because `on_connect()` is
    not supposed to block. */
    coro_t::spawn_sometime(boost::bind(
        &semilattice_manager_t<metadata_t>::send_metadata_to_peer, this,
        peer, metadata, metadata_version, auto_drainer_t::lock_t(drainers.get())
        ));
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::on_disconnect(peer_id_t) {
    assert_thread();

    /* ignore */
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::send_metadata_to_peer(peer_id_t peer, metadata_t m, metadata_version_t mv, auto_drainer_t::lock_t) {
    message_service->send_message(peer,
        boost::bind(&semilattice_manager_t<metadata_t>::write_metadata, _1, m, mv));
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::deliver_metadata_on_home_thread(peer_id_t sender, metadata_t md, metadata_version_t mv, auto_drainer_t::lock_t) {
    on_thread_t thread_switcher(home_thread());
    join_metadata_locally(md);
    mutex_assertion_t::acq_t acq(&peer_version_mutex);
    std::pair<typename std::map<peer_id_t, metadata_version_t>::iterator, bool> inserted =
        last_versions_seen.insert(std::make_pair(sender, mv));
    if (!inserted.second) {
        inserted.first->second = std::max(inserted.first->second, mv);
    }
    for (typename std::multimap<std::pair<peer_id_t, metadata_version_t>, cond_t *>::iterator it = version_waiters.begin();
            it != version_waiters.end(); it++) {
        if (it->first.first == sender && it->first.second <= mv && !it->second->is_pulsed()) {
            it->second->pulse();
        }
    }
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::deliver_sync_from_query_on_home_thread(peer_id_t sender, sync_from_query_id_t query_id, auto_drainer_t::lock_t) {
    on_thread_t thread_switcher(home_thread());
    message_service->send_message(sender,
        boost::bind(&semilattice_manager_t<metadata_t>::write_sync_from_reply, _1, query_id, metadata_version));
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::deliver_sync_from_reply_on_home_thread(UNUSED peer_id_t sender, sync_from_query_id_t query_id, metadata_version_t version, auto_drainer_t::lock_t) {
    on_thread_t thread_switcher(home_thread());
    std::map<sync_from_query_id_t, promise_t<metadata_version_t> *>::iterator it = sync_from_waiters.find(query_id);
    if (it != sync_from_waiters.end()) {
        if (it->second->get_ready_signal()->is_pulsed()) {
            logWRN("Got duplicate reply to a sync_from() call. TCP checksum failure?");
        } else {
            it->second->pulse(version);
        }
    }
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::deliver_sync_to_query_on_home_thread(peer_id_t sender, sync_to_query_id_t query_id, metadata_version_t version, auto_drainer_t::lock_t keepalive) {
    cross_thread_signal_t cross_thread_interruptor(keepalive.get_drain_signal(), home_thread());
    on_thread_t thread_switcher(home_thread());
    try {
        wait_for_version_from_peer(sender, version, &cross_thread_interruptor);
    } catch (interrupted_exc_t) {
        return;
    } catch (sync_failed_exc_t) {
        return;
    }
    message_service->send_message(sender,
        boost::bind(&semilattice_manager_t<metadata_t>::write_sync_to_reply, _1, query_id));
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::deliver_sync_to_reply_on_home_thread(UNUSED peer_id_t sender, sync_to_query_id_t query_id, auto_drainer_t::lock_t) {
    on_thread_t thread_switcher(home_thread());
    std::map<sync_to_query_id_t, cond_t *>::iterator it = sync_to_waiters.find(query_id);
    if (it != sync_to_waiters.end()) {
        if (it->second->is_pulsed()) {
            logWRN("Got duplicate reply to a sync_to() call. TCP checksum failure?");
        } else {
            it->second->pulse();
        }
    }
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::call_function_with_no_args(const boost::function<void()> &fun) {
    fun();
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::join_metadata_locally(metadata_t added_metadata) {
    assert_thread();
    rwi_lock_assertion_t::write_acq_t acq(&metadata_mutex);
    semilattice_join(&metadata, added_metadata);
    metadata_publisher.publish(&semilattice_manager_t<metadata_t>::call_function_with_no_args);
}

template<class metadata_t>
void semilattice_manager_t<metadata_t>::wait_for_version_from_peer(peer_id_t peer, metadata_version_t version, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t, sync_failed_exc_t) {
    assert_thread();
    mutex_assertion_t::acq_t acq(&peer_version_mutex);
    typename std::map<peer_id_t, metadata_version_t>::iterator it = last_versions_seen.find(peer);
    if (it != last_versions_seen.end() && it->second >= version) {
        return;
    }
    cond_t caught_up;
    multimap_insertion_sentry_t<std::pair<peer_id_t, metadata_version_t>, cond_t *> insertion_sentry(
        &version_waiters, std::make_pair(peer, version), &caught_up);
    acq.reset();
    disconnect_watcher_t watcher(message_service->get_connectivity_service(), peer);
    wait_any_t waiter(&caught_up, &watcher);
    wait_interruptible(&waiter, interruptor);
    if (watcher.is_pulsed()) {
        throw sync_failed_exc_t();
    }
    rassert(caught_up.is_pulsed());
}
