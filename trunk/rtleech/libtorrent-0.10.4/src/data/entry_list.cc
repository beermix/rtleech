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

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <rak/error_number.h>
#include <rak/file_stat.h>
#include <rak/fs_stat.h>
#include <rak/functional.h>

#include "torrent/exceptions.h"
#include "torrent/path.h"

#include "chunk.h"
#include "file_meta.h"
#include "entry_list.h"
#include "memory_chunk.h"

namespace torrent {

void
EntryList::push_back(const Path& path, const EntryListNode::Range& range, off_t size) {
  if (sizeof(off_t) != 8)
    throw internal_error("sizeof(off_t) != 8");

  if (size + m_bytesSize < m_bytesSize)
    throw internal_error("Sum of files added to EntryList overflowed 64bit");

  EntryListNode* e = new EntryListNode();

  e->set_position(m_bytesSize);
  e->set_size(size);
  e->set_range(range);

  *e->path() = path;

  base_type::push_back(e);

  m_bytesSize += size;
}

void
EntryList::clear() {
  close();

  std::for_each(begin(), end(), rak::call_delete<EntryListNode>());

  base_type::clear();
  m_bytesSize = 0;
}

void
EntryList::open() {
  if (m_rootDir.empty())
    throw internal_error("EntryList::open() m_rootDir.empty().");

  m_indirectLinks.push_back(m_rootDir);

  Path lastPath;
  iterator itr = begin();

  try {
    if (::mkdir(m_rootDir.c_str(), 0777) && errno != EEXIST)
      throw storage_error("Could not create directory '" + m_rootDir + "': " + strerror(errno));
  
    while (itr != end()) {
      EntryListNode* entry = *itr++;

      if (entry->file_meta()->is_open())
        throw internal_error("EntryList::open(...) found an already opened file.");
      
      m_slotInsertFileMeta(entry->file_meta());

      if (entry->path()->empty())
        throw storage_error("Found an empty filename.");

      if (!open_file(&*entry, lastPath))
        throw storage_error("Could not open file \"" + m_rootDir + entry->path()->as_string() + "\": " + rak::error_number::current().c_str());
      
      lastPath = *entry->path();
    }

  } catch (storage_error& e) {
    for (iterator cleanupItr = begin(); cleanupItr != itr; ++cleanupItr)
      m_slotEraseFileMeta((*cleanupItr)->file_meta());

    throw e;
  }

  m_isOpen = true;
}

void
EntryList::close() {
  if (!m_isOpen)
    return;

  for (iterator itr = begin(), last = end(); itr != last; ++itr) {
    m_slotEraseFileMeta((*itr)->file_meta());
    
    (*itr)->set_completed(0);
  }

  m_isOpen = false;
  m_indirectLinks.clear();
}

void
EntryList::set_root_dir(const std::string& path) {
  std::string::size_type last = path.find_last_not_of('/');

  if (last == std::string::npos)
    m_rootDir = ".";
  else
    m_rootDir = path.substr(0, last + 1);

  for (iterator itr = begin(), last = end(); itr != last; ++itr) {
    if ((*itr)->file_meta()->is_open())
      throw internal_error("EntryList::set_root_dir(...) found an already opened file.");
    
    (*itr)->file_meta()->set_path(m_rootDir + (*itr)->path()->as_string());
  }
}

bool
EntryList::resize_all() {
  iterator itr = std::find_if(begin(), end(), std::not1(std::mem_fun(&EntryListNode::resize_file)));

  if (itr != end()) {
    std::for_each(++itr, end(), std::mem_fun(&EntryListNode::resize_file));
    return false;
  }

  return true;
}

EntryList::iterator
EntryList::at_position(iterator itr, off_t offset) {
  while (itr != end())
    if (offset >= (*itr)->position() + (*itr)->size())
      ++itr;
    else
      return itr;

  return itr;
}

inline void
EntryList::make_directory(Path::const_iterator pathBegin, Path::const_iterator pathEnd, Path::const_iterator startItr) {
  std::string path = m_rootDir;

  while (pathBegin != pathEnd) {
    path += "/" + *pathBegin;

    if (pathBegin++ != startItr)
      continue;

    startItr++;

    rak::file_stat fileStat;

    if (fileStat.update_link(path) &&
        fileStat.is_link() &&
        std::find(m_indirectLinks.begin(), m_indirectLinks.end(), path) == m_indirectLinks.end())
      m_indirectLinks.push_back(path);

    if (pathBegin == pathEnd)
      break;

    if (::mkdir(path.c_str(), 0777) && errno != EEXIST)
      throw storage_error("Could not create directory '" + path + "': " + strerror(errno));
  }
}

bool
EntryList::open_file(EntryListNode* node, const Path& lastPath) {
  const Path* path = node->path();

  Path::const_iterator lastItr = lastPath.begin();
  Path::const_iterator firstMismatch = path->begin();

  // Couldn't find a suitable stl algo, need to write my own.
  while (firstMismatch != path->end() && lastItr != lastPath.end() && *firstMismatch == *lastItr) {
    lastItr++;
    firstMismatch++;
  }

  make_directory(path->begin(), path->end(), firstMismatch);

  // Some torrents indicate an empty directory by having a path with
  // an empty last element. This entry must be zero length.
  if (path->back().empty())
    return node->size() == 0;

  return
    node->file_meta()->prepare(MemoryChunk::prot_read | MemoryChunk::prot_write, SocketFile::o_create) ||
    node->file_meta()->prepare(MemoryChunk::prot_read, SocketFile::o_create);
}

// This function should really ensure that we arn't dealing files
// spread over multiple mount-points.
uint64_t
EntryList::free_diskspace() const {
  uint64_t freeDiskspace = std::numeric_limits<uint64_t>::max();

  for (path_list::const_iterator itr = m_indirectLinks.begin(), last = m_indirectLinks.end(); itr != last; ++itr) {
    rak::fs_stat stat;

    if (!stat.update(*itr))
      continue;

    freeDiskspace = std::min<uint64_t>(freeDiskspace, stat.bytes_avail());
  }

  return freeDiskspace != std::numeric_limits<uint64_t>::max() ? freeDiskspace : 0;
}

inline MemoryChunk
EntryList::create_chunk_part(iterator itr, off_t offset, uint32_t length, int prot) {
  offset -= (*itr)->position();
  length = std::min<off_t>(length, (*itr)->size() - offset);

  if (offset < 0)
    throw internal_error("EntryList::chunk_part(...) caught a negative offset");

  // Check that offset != length of file.

  if (!(*itr)->file_meta()->prepare(prot))
    return MemoryChunk();

  return (*itr)->file_meta()->get_file().create_chunk(offset, length, prot, MemoryChunk::map_shared);
}

Chunk*
EntryList::create_chunk(off_t offset, uint32_t length, int prot) {
  if (offset + length > m_bytesSize)
    throw internal_error("Tried to access chunk out of range in EntryList");

  std::auto_ptr<Chunk> chunk(new Chunk);

  for (iterator itr = std::find_if(begin(), end(), std::bind2nd(std::mem_fun(&EntryListNode::is_valid_position), offset)); length != 0; ++itr) {

    if (itr == end())
      throw internal_error("EntryList could not find a valid file for chunk");

    if ((*itr)->size() == 0)
      continue;

    MemoryChunk mc = create_chunk_part(itr, offset, length, prot);

    if (!mc.is_valid())
      return NULL;

    if (mc.size() == 0)
      throw internal_error("EntryList::create_chunk(...) mc.size() == 0.");

    if (mc.size() > length)
      throw internal_error("EntryList::create_chunk(...) mc.size() > length.");

    chunk->push_back(ChunkPart::MAPPED_MMAP, mc);

    offset += mc.size();
    length -= mc.size();
  }

  if (chunk->empty())
    return NULL;

  return chunk.release();
}

}