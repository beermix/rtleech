// libTorrent - BitTorrent library
// Copyright (C) 2005-2006, Jari Sundell
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// In addition, as a special exception, the copyright holders give
// permission to link the code of portions of this program with the
// OpenSSL library under certain conditions as described in each
// individual source file, and distribute linked combinations
// including the two.
//
// You must obey the GNU General Public License in all respects for
// all of the code used other than OpenSSL.  If you modify file(s)
// with this exception, you may extend this exception to your version
// of the file(s), but you are not obligated to do so.  If you do not
// wish to do so, delete this exception statement from your version.
// If you delete this exception statement from all source files in the
// program, then also delete it here.
//
// Contact:  Jari Sundell <jaris@ifi.uio.no>
//
//           Skomakerveien 33
//           3185 Skoppum, NORWAY

#include "config.h"

#include <rak/functional.h>
#include <sigc++/bind.h>
#include <sigc++/hide.h>

#include "data/chunk_list.h"
#include "data/hash_queue.h"
#include "data/hash_torrent.h"
#include "download/available_list.h"
#include "download/choke_manager.h"
#include "download/chunk_selector.h"
#include "download/chunk_statistics.h"
#include "download/download_wrapper.h"
#include "protocol/peer_connection_base.h"
#include "protocol/peer_factory.h"
#include "download/download_info.h"
#include "tracker/tracker_manager.h"

#include "exceptions.h"
#include "block.h"
#include "block_list.h"
#include "download.h"
#include "file_list.h"
#include "object.h"
#include "peer_info.h"
#include "tracker_list.h"

namespace torrent {

void
Download::open() {
  m_ptr->open();
}

void
Download::close() {
  if (m_ptr->info()->is_active())
    stop();

  m_ptr->close();
}

void
Download::start() {
  if (!m_ptr->hash_checker()->is_checked())
    throw client_error("Tried to start an unchecked download");

  if (!m_ptr->info()->is_open())
    throw client_error("Tried to start a closed download");

  if (m_ptr->info()->is_active())
    return;

  m_ptr->main()->start();
  m_ptr->main()->tracker_manager()->set_active(true);

  // Either the client queued a completed request, or it is still
  // sending the stopped request. Don't send started nor reset the
  // baseline.
  //
  // Check for stopped request.
  if (m_ptr->main()->tracker_manager()->is_busy())
    return;

  // Reset the uploaded/download baseline when we restart the download
  // so that broken trackers get the right uploaded ratio.
  m_ptr->info()->set_uploaded_baseline(m_ptr->info()->up_rate()->total());
  m_ptr->info()->set_completed_baseline(m_ptr->info()->slot_completed()());

  m_ptr->main()->tracker_manager()->send_start();
}

void
Download::stop() {
  if (!m_ptr->info()->is_active())
    return;

  m_ptr->main()->stop();
  m_ptr->main()->tracker_manager()->set_active(false);
  m_ptr->main()->tracker_manager()->send_stop();
}

bool
Download::hash_check(bool tryQuick) {
  if (m_ptr->hash_checker()->is_checking())
    return m_ptr->hash_checker()->start(tryQuick);

  if (!m_ptr->info()->is_open() || m_ptr->info()->is_active())
    throw client_error("Download::hash_check(...) called on a closed or active download.");

  if (m_ptr->hash_checker()->is_checked())
    throw client_error("Download::hash_check(...) called but already hash checked.");

  // The bitfield still hasn't been allocated, so no resume data was
  // given. 

  if (m_ptr->main()->content()->bitfield()->empty()) {
    m_ptr->main()->content()->bitfield()->allocate();
    m_ptr->main()->content()->bitfield()->unset_all();

    m_ptr->hash_checker()->ranges().insert(0, m_ptr->main()->content()->chunk_total());

    // Make sure other book-keeping is cleared, like file progress.

  } else {
    m_ptr->main()->content()->update_done();
  }

  return m_ptr->hash_checker()->start(tryQuick);
}

// Propably not correct, need to clear content, etc.
void
Download::hash_stop() {
  if (!m_ptr->hash_checker()->is_checking())
    return;

  // Stop the hashing first as we need to make sure all chunks are
  // released when DownloadMain::close() is called.
  m_ptr->hash_checker()->clear();

  // Clear after m_hash to ensure that the empty hash done signal does
  // not get passed to HashTorrent.
  m_ptr->hash_checker()->get_queue()->remove(m_ptr);
}

bool
Download::is_open() const {
  return m_ptr->info()->is_open();
}

bool
Download::is_active() const {
  return m_ptr->info()->is_active();
}

bool
Download::is_hash_checked() const {
  return m_ptr->hash_checker()->is_checked();
}

bool
Download::is_hash_checking() const {
  return m_ptr->hash_checker()->is_checking();
}

const std::string&
Download::name() const {
  if (m_ptr == NULL)
    throw internal_error("Download::name() m_ptr == NULL.");

  return m_ptr->info()->name();
}

const std::string&
Download::info_hash() const {
  if (m_ptr == NULL)
    throw internal_error("Download::info_hash() m_ptr == NULL.");

  return m_ptr->info()->hash();
}

const std::string&
Download::local_id() const {
  if (m_ptr == NULL)
    throw internal_error("Download::local_id() m_ptr == NULL.");

  return m_ptr->info()->local_id();
}

uint32_t
Download::creation_date() const {
  if (m_ptr->bencode()->has_key_value("creation date"))
    return m_ptr->bencode()->get_key_value("creation date");
  else
    return 0;
}

Object*
Download::bencode() {
  return m_ptr->bencode();
}

const Object*
Download::bencode() const {
  return m_ptr->bencode();
}

FileList
Download::file_list() const {
  return FileList(m_ptr->main()->content()->entry_list());
}

TrackerList
Download::tracker_list() const {
  return TrackerList(m_ptr->main()->tracker_manager());
}

PeerList*
Download::peer_list() {
  return m_ptr->main()->peer_list();
}

const PeerList*
Download::peer_list() const {
  return m_ptr->main()->peer_list();
}
const TransferList*
Download::transfer_list() const {
  return m_ptr->main()->delegator()->transfer_list();
}

Rate*
Download::down_rate() {
  return m_ptr->info()->down_rate();
}

const Rate*
Download::down_rate() const {
  return m_ptr->info()->down_rate();
}

Rate*
Download::up_rate() {
  return m_ptr->info()->up_rate();
}

const Rate*
Download::up_rate() const {
  return m_ptr->info()->up_rate();
}

uint64_t
Download::bytes_done() const {
  uint64_t a = 0;
 
  Delegator* d = m_ptr->main()->delegator();

  for (TransferList::const_iterator itr1 = d->transfer_list()->begin(), last1 = d->transfer_list()->end(); itr1 != last1; ++itr1)
    for (BlockList::const_iterator itr2 = (*itr1)->begin(), last2 = (*itr1)->end(); itr2 != last2; ++itr2)
      if (itr2->is_finished())
        a += itr2->piece().length();
  
  return a + m_ptr->main()->content()->bytes_completed();
}

uint64_t
Download::bytes_total() const {
  return m_ptr->main()->content()->entry_list()->bytes_size();
}

uint64_t
Download::free_diskspace() const {
  return m_ptr->main()->content()->entry_list()->free_diskspace();
}

uint32_t
Download::chunks_size() const {
  return m_ptr->main()->content()->chunk_size();
}

uint32_t
Download::chunks_done() const {
  return m_ptr->main()->content()->chunks_completed();
}

uint32_t 
Download::chunks_total() const {
  return m_ptr->main()->content()->chunk_total();
}

uint32_t 
Download::chunks_hashed() const {
  return m_ptr->hash_checker()->position();
}

const uint8_t*
Download::chunks_seen() const {
  return !m_ptr->main()->chunk_statistics()->empty() ? &*m_ptr->main()->chunk_statistics()->begin() : NULL;
}

void
Download::set_chunks_done(uint32_t chunks) {
  if (m_ptr->info()->is_open())
    throw input_error("Download::set_chunks_done(...) Download is open.");

  m_ptr->main()->content()->bitfield()->set_size_set(chunks);
}

void
Download::set_bitfield(bool allSet) {
  if (m_ptr->hash_checker()->is_checked() || m_ptr->hash_checker()->is_checking())
    throw input_error("Download::set_bitfield(...) Download in invalid state.");

  Bitfield* bitfield = m_ptr->main()->content()->bitfield();

  bitfield->allocate();

  if (allSet)
    bitfield->set_all();
  else
    bitfield->unset_all();
  
  m_ptr->hash_checker()->ranges().clear();
}

void
Download::set_bitfield(uint8_t* first, uint8_t* last) {
  if (m_ptr->hash_checker()->is_checked() || m_ptr->hash_checker()->is_checking())
    throw input_error("Download::set_bitfield(...) Download in invalid state.");

  if (std::distance(first, last) != (ptrdiff_t)m_ptr->main()->content()->bitfield()->size_bytes())
    throw input_error("Download::set_bitfield(...) Invalid length.");

  Bitfield* bitfield = m_ptr->main()->content()->bitfield();

  bitfield->allocate();
  std::memcpy(bitfield->begin(), first, bitfield->size_bytes());
  bitfield->update();
  
  m_ptr->hash_checker()->ranges().clear();
}

void
Download::clear_range(uint32_t first, uint32_t last) {
  if (m_ptr->hash_checker()->is_checked() || m_ptr->hash_checker()->is_checking() || m_ptr->main()->content()->bitfield()->empty())
    throw input_error("Download::clear_range(...) Download in invalid state.");

  m_ptr->hash_checker()->ranges().insert(first, last);
  m_ptr->main()->content()->bitfield()->unset_range(first, last);
}
 
const Bitfield*
Download::bitfield() const {
  return m_ptr->main()->content()->bitfield();
}

void
Download::sync_chunks() {
  m_ptr->main()->chunk_list()->sync_chunks(ChunkList::sync_all | ChunkList::sync_force);
}

uint32_t
Download::peers_min() const {
  return m_ptr->main()->connection_list()->get_min_size();
}

uint32_t
Download::peers_max() const {
  return m_ptr->main()->connection_list()->get_max_size();
}

uint32_t
Download::peers_connected() const {
  return m_ptr->main()->connection_list()->size();
}

uint32_t
Download::peers_not_connected() const {
  return m_ptr->main()->peer_list()->available_list()->size();
}

uint32_t
Download::peers_complete() const {
  return m_ptr->main()->chunk_statistics()->complete();
}

uint32_t
Download::peers_accounted() const {
  return m_ptr->main()->chunk_statistics()->accounted();
}

uint32_t
Download::peers_currently_unchoked() const {
  return m_ptr->main()->choke_manager()->currently_unchoked();
}

uint32_t
Download::peers_currently_interested() const {
  return m_ptr->main()->choke_manager()->currently_interested();
}

bool
Download::accepting_new_peers() const {
  return m_ptr->info()->is_accepting_new_peers();
}

uint32_t
Download::uploads_max() const {
  return m_ptr->main()->choke_manager()->max_unchoked();
}
  
void
Download::set_peers_min(uint32_t v) {
  if (v > (1 << 16))
    throw input_error("Min peer connections must be between 0 and 2^16.");
  
  m_ptr->main()->connection_list()->set_min_size(v);
  m_ptr->main()->receive_connect_peers();
}

void
Download::set_peers_max(uint32_t v) {
  if (v > (1 << 16))
    throw input_error("Max peer connections must be between 0 and 2^16.");

  m_ptr->main()->connection_list()->set_max_size(v);
}

void
Download::set_uploads_max(uint32_t v) {
  if (v > (1 << 16))
    throw input_error("Max uploads must be between 0 and 2^16.");

  m_ptr->main()->choke_manager()->set_max_unchoked(v);
  m_ptr->main()->choke_manager()->balance();
}

Download::ConnectionType
Download::connection_type() const {
  return (ConnectionType)m_ptr->connection_type();
}

void
Download::set_connection_type(ConnectionType t) {
  switch (t) {
  case CONNECTION_LEECH:
    m_ptr->main()->connection_list()->slot_new_connection(&createPeerConnectionDefault);
    break;
  case CONNECTION_SEED:
    m_ptr->main()->connection_list()->slot_new_connection(&createPeerConnectionSeed);
    break;
  default:
    throw input_error("torrent::Download::set_connection_type(...) received an unknown type.");
  };

  m_ptr->set_connection_type(t);
}

void
Download::update_priorities() {
  m_ptr->receive_update_priorities();
}

void
Download::peer_list(PList& pList) {
  std::for_each(m_ptr->main()->connection_list()->begin(), m_ptr->main()->connection_list()->end(),
                rak::bind1st(std::mem_fun<void,PList,PList::const_reference>(&PList::push_back), &pList));
}

Peer
Download::peer_find(const std::string& id) {
  ConnectionList::iterator itr =
    std::find_if(m_ptr->main()->connection_list()->begin(), m_ptr->main()->connection_list()->end(),
                 rak::equal(id, rak::on(std::mem_fun(&PeerConnectionBase::c_peer_info), std::mem_fun(&PeerInfo::id))));

  return itr != m_ptr->main()->connection_list()->end() ? *itr : NULL;
}

void
Download::disconnect_peer(Peer p) {
  m_ptr->main()->connection_list()->erase(p.ptr(), 0);
}

sigc::connection
Download::signal_download_done(Download::slot_void_type s) {
  return m_ptr->signal_download_done().connect(s);
}

sigc::connection
Download::signal_hash_done(Download::slot_void_type s) {
  return m_ptr->signal_initial_hash().connect(s);
}

sigc::connection
Download::signal_peer_connected(Download::slot_peer_type s) {
  return m_ptr->signal_peer_connected().connect(s);
}

sigc::connection
Download::signal_peer_disconnected(Download::slot_peer_type s) {
  return m_ptr->signal_peer_disconnected().connect(s);
}

sigc::connection
Download::signal_tracker_succeded(Download::slot_void_type s) {
  return m_ptr->signal_tracker_success().connect(s);
}

sigc::connection
Download::signal_tracker_failed(Download::slot_string_type s) {
  return m_ptr->signal_tracker_failed().connect(s);
}

sigc::connection
Download::signal_tracker_dump(Download::slot_dump_type s) {
  return m_ptr->info()->signal_tracker_dump().connect(s);
}

sigc::connection
Download::signal_chunk_passed(Download::slot_chunk_type s) {
  return m_ptr->signal_chunk_passed().connect(s);
}

sigc::connection
Download::signal_chunk_failed(Download::slot_chunk_type s) {
  return m_ptr->signal_chunk_failed().connect(s);
}

sigc::connection
Download::signal_network_log(slot_string_type s) {
  return m_ptr->info()->signal_network_log().connect(s);
}

sigc::connection
Download::signal_storage_error(slot_string_type s) {
  return m_ptr->info()->signal_storage_error().connect(s);
}

}
