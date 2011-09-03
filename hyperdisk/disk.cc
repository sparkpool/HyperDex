// Copyright (c) 2011, Cornell University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of HyperDex nor the names of its contributors may be
//       used to endorse or promote products derived from this software without
//       specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#define __STDC_LIMIT_MACROS

// C
#include <cstdio>

// POSIX
#include <sys/stat.h>
#include <sys/types.h>

// e
#include <e/guard.h>

// Utils
#include "bithacks.h"
#include "hashing.h"

// HyperDisk
#include "coordinate.h"
#include "hyperdisk/disk.h"
#include "log_entry.h"
#include "shard.h"
#include "shard_snapshot.h"
#include "shard_vector.h"

// LOCKING:  IF YOU DO ANYTHING WITH THIS CODE, READ THIS FIRST!
//
// At any given time, only one thread should be mutating shards.  In this
// context a mutation to a shard may be either a PUT/DEL, or
// cleaning/splitting/joining the shard. The m_shard_mutate mutex is used to
// enforce this constraint.
//
// Certain mutations require changing the shard_vector (e.g., to replace a shard
// with its equivalent that has had dead space collected).  These mutations
// conflict with reading from the shards (e.g. for a GET).  To that end, the
// m_shards_lock is a reader-writer lock which provides this synchronization
// between the readers and single mutator.  We know that there is a single
// mutator because of the above reasoning.  It is safe for the single mutator to
// grab a reference to m_shards while holding m_shard_mutate without grabbing a
// read lock on m_shards_lock.  The mutator must grab a write lock when changing
// the shards.
//
// Note that synchronization around m_shards revolves around the
// reference-counted *pointer* to a shard_vector, and not the shard_vector
// itself.  Methods which access the shard_vector are responsible for ensuring
// proper synchronization.  GET does this by allowing races in shard_vector
// accesses, but using the WAL to detect them.  PUT/DEL do this by writing to
// the WAL.  Trickle does this by using locking when exchanging the
// shard_vectors.

hyperdisk :: disk :: disk(const po6::pathname& directory, uint16_t arity)
    : m_ref(0)
    , m_arity(arity)
    , m_shards_mutate()
    , m_shards_lock()
    , m_shards()
    , m_log()
    , m_base()
    , m_base_filename(directory)
    , m_spare_shards_lock()
    , m_spare_shards()
    , m_spare_shard_counter(0)
{
    if (mkdir(directory.get(), S_IRWXU) < 0 && errno != EEXIST)
    {
        throw po6::error(errno);
    }

    m_base = open(directory.get(), O_RDONLY);

    if (m_base.get() < 0)
    {
        throw po6::error(errno);
    }

    // Create a starting disk which holds everything.
    po6::threads::mutex::hold a(&m_shards_mutate);
    po6::threads::mutex::hold b(&m_shards_lock);
    coordinate start(0, 0, 0, 0);
    e::intrusive_ptr<shard> s = create_shard(start);
    m_shards = new shard_vector(start, s);
}

hyperdisk :: disk :: ~disk() throw ()
{
}

hyperdisk::returncode
hyperdisk :: disk :: get(const e::buffer& key,
                         std::vector<e::buffer>* value,
                         uint64_t* version)
{
    coordinate coord = get_coordinate(key);
    returncode shard_res = NOTFOUND;
    e::intrusive_ptr<shard_vector> shards;
    e::locking_iterable_fifo<log_entry>::iterator it = m_log.iterate();

    {
        po6::threads::mutex::hold b(&m_shards_lock);
        shards = m_shards;
    }

    for (size_t i = 0; i < shards->size(); ++i)
    {
        if (!shards->get_coordinate(i).primary_contains(coord))
        {
            continue;
        }

        shard_res = shards->get_shard(i)->get(coord.primary_hash, key, value, version);

        if (shard_res == SUCCESS)
        {
            break;
        }
    }

    bool found = false;
    returncode wal_res = NOTFOUND;

    for (; it.valid(); it.next())
    {
        if (it->coord.primary_contains(coord) && it->key == key)
        {
            if (it->coord.secondary_mask == UINT32_MAX)
            {
                assert(it->coord.primary_mask == UINT32_MAX);
                assert(it->coord.secondary_mask == UINT32_MAX);
                *value = it->value;
                *version = it->version;
                wal_res = SUCCESS;
            }
            else
            {
                assert(it->coord.primary_mask == UINT32_MAX);
                assert(it->coord.secondary_mask == 0);
                wal_res = NOTFOUND;
            }

            found = true;
        }
    }

    if (found)
    {
        return wal_res;
    }

    return shard_res;
}

hyperdisk::returncode
hyperdisk :: disk :: put(const e::buffer& key,
                         const std::vector<e::buffer>& value,
                         uint64_t version)
{
    if (value.size() + 1 != m_arity)
    {
        return WRONGARITY;
    }

    coordinate coord = get_coordinate(key, value);
    m_log.append(log_entry(coord, key, value, version));
    return SUCCESS;
}

hyperdisk::returncode
hyperdisk :: disk :: del(const e::buffer& key)
{
    coordinate coord = get_coordinate(key);
    m_log.append(log_entry(coord, key));
    return SUCCESS;
}

e::intrusive_ptr<hyperdisk::snapshot>
hyperdisk :: disk :: make_snapshot()
{
    assert(!"Not implemented."); // XXX
}

e::intrusive_ptr<hyperdisk::rolling_snapshot>
hyperdisk :: disk :: make_rolling_snapshot()
{
    assert(!"Not implemented."); // XXX
}

hyperdisk::returncode
hyperdisk :: disk :: drop()
{
    po6::threads::mutex::hold a(&m_shards_mutate);
    po6::threads::mutex::hold b(&m_shards_lock);
    returncode ret = SUCCESS;
    e::intrusive_ptr<shard_vector> shards = m_shards;

    for (size_t i = 0; i < shards->size(); ++i)
    {
        if (drop_shard(shards->get_coordinate(i)) != SUCCESS)
        {
            ret = DROPFAILED;
        }
    }

    if (ret == SUCCESS)
    {
        if (rmdir(m_base_filename.get()) < 0)
        {
            ret = DROPFAILED;
        }
    }

    return ret;
}

// This operation will return SUCCESS as long as it knows that progress is being
// made.  Practically this means that if it encounters a full disk, it will
// deal with the full disk and return without moving any data to the newly
// changed disks.  In practice, several threads will be hammering this method to
// push data to disk, so we can expect that not doing the work will not be too
// costly.
hyperdisk::returncode
hyperdisk :: disk :: flush()
{
    if (!m_shards_mutate.trylock())
    {
        return SUCCESS;
    }

    e::guard hold = e::makeobjguard(m_shards_mutate, &po6::threads::mutex::unlock);

    for (size_t i = 0; i < 100 && !m_log.empty(); ++i)
    {
        bool deleted = false;
        const coordinate& coord = m_log.oldest().coord;
        const e::buffer& key = m_log.oldest().key;

        for (size_t i = 0; !deleted && i < m_shards->size(); ++i)
        {
            if (!m_shards->get_coordinate(i).primary_contains(coord))
            {
                continue;
            }

            switch (m_shards->get_shard(i)->del(coord.primary_hash, key))
            {
                case SUCCESS:
                    deleted = true;
                    break;
                case NOTFOUND:
                    break;
                case DATAFULL:
                    return deal_with_full_shard(i);
                case WRONGARITY:
                case HASHFULL:
                case SEARCHFULL:
                case SYNCFAILED:
                case DROPFAILED:
                case MISSINGDISK:
                default:
                    assert(!"Programming error.");
            }
        }

        if (coord.secondary_mask == UINT32_MAX)
        {
            bool inserted = false;
            const std::vector<e::buffer>& value = m_log.oldest().value;
            const uint64_t version = m_log.oldest().version;

            // We must start at the end and work backwards.
            for (ssize_t i = m_shards->size() - 1; !inserted && i >= 0; --i)
            {
                if (!m_shards->get_coordinate(i).contains(coord))
                {
                    continue;
                }

                switch (m_shards->get_shard(i)->put(coord.primary_hash, coord.secondary_hash,
                                                    key, value, version))
                {
                    case SUCCESS:
                        inserted = true;
                        break;
                    case DATAFULL:
                    case HASHFULL:
                    case SEARCHFULL:
                        return deal_with_full_shard(i);
                    case MISSINGDISK:
                    case NOTFOUND:
                    case WRONGARITY:
                    case SYNCFAILED:
                    case DROPFAILED:
                    default:
                        assert(!"Programming error.");
                }
            }
        }

        m_log.remove_oldest();
    }

    return SUCCESS;
}

hyperdisk::returncode
hyperdisk :: disk :: preallocate()
{
    {
        po6::threads::mutex::hold hold(&m_spare_shards_lock);

        if (m_spare_shards.size() >= 16)
        {
            return SUCCESS;
        }
    }

    e::intrusive_ptr<shard_vector> shards;

    {
        po6::threads::mutex::hold hold(&m_shards_lock);
        shards = m_shards;
    }

    size_t num_shards = 0;

    for (size_t i = 0; i < shards->size(); ++i)
    {
        shard* s = shards->get_shard(i);
        int stale = s->stale_space();
        int free = s->free_space();

        // There is no describable reason for picking these except that you can
        // be pretty sure that enough shards will exist to do splits.  That
        // being said, this will waste space when shards are mostly full.  Feel
        // free to tune this using logic and reason and submit a patch.

        if (free <= 25)
        {
            num_shards += 0;
        }
        else if (free <= 50)
        {
            num_shards += 1;
        }
        else if (free <= 75)
        {
            if (stale >= 30)
            {
                num_shards += 1;
            }
            else
            {
                num_shards += 2;
            }
        }
        else
        {
            if (stale >= 30)
            {
                num_shards += 1;
            }
            else
            {
                num_shards += 4;
            }
        }
    }

    size_t spare_shards_needed;

    {
        po6::threads::mutex::hold hold(&m_spare_shards_lock);
        spare_shards_needed = num_shards;
        num_shards = std::min(num_shards, spare_shards_needed - m_spare_shards.size());
    }

    for (size_t i = 0; i < num_shards; ++i)
    {
        std::ostringstream ostr;

        {
            po6::threads::mutex::hold hold(&m_spare_shards_lock);
            ostr << "spare-" << m_spare_shard_counter;
            ++m_spare_shard_counter;
        }

        po6::pathname sparepath(ostr.str());
        e::intrusive_ptr<hyperdisk::shard> spareshard = hyperdisk::shard::create(m_base, sparepath);

        {
            po6::threads::mutex::hold hold(&m_spare_shards_lock);
            m_spare_shards.push(std::make_pair(sparepath, spareshard));
            num_shards = std::min(num_shards, spare_shards_needed - (m_spare_shards.size() - i));
        }
    }
}

hyperdisk::returncode
hyperdisk :: disk :: async()
{
    e::intrusive_ptr<shard_vector> shards;
    returncode ret = SUCCESS;

    {
        po6::threads::mutex::hold b(&m_shards_lock);
        shards = m_shards;
    }

    for (size_t i = 0; i < shards->size(); ++i)
    {
        if (shards->get_shard(i)->async() != SUCCESS)
        {
            ret = SYNCFAILED;
        }
    }

    return ret;
}

hyperdisk::returncode
hyperdisk :: disk :: sync()
{
    e::intrusive_ptr<shard_vector> shards;
    returncode ret = SUCCESS;

    {
        po6::threads::mutex::hold b(&m_shards_lock);
        shards = m_shards;
    }

    for (size_t i = 0; i < shards->size(); ++i)
    {
        if (shards->get_shard(i)->sync() != SUCCESS)
        {
            ret = SYNCFAILED;
        }
    }

    return ret;
}

po6::pathname
hyperdisk :: disk :: shard_filename(const coordinate& c)
{
    std::ostringstream ostr;
    ostr << std::hex << std::setfill('0') << std::setw(16) << c.primary_mask;
    ostr << "-" << std::setw(16) << c.primary_hash;
    ostr << "-" << std::setw(16) << c.secondary_mask;
    ostr << "-" << std::setw(16) << c.secondary_hash;
    return po6::pathname(ostr.str());
}

po6::pathname
hyperdisk :: disk :: shard_tmp_filename(const coordinate& c)
{
    std::ostringstream ostr;
    ostr << std::hex << std::setfill('0') << std::setw(16) << c.primary_mask;
    ostr << "-" << std::setw(16) << c.primary_hash;
    ostr << "-" << std::setw(16) << c.secondary_mask;
    ostr << "-" << std::setw(16) << c.secondary_hash;
    ostr << "-tmp";
    return po6::pathname(ostr.str());
}

e::intrusive_ptr<hyperdisk::shard>
hyperdisk :: disk :: create_shard(const coordinate& c)
{
    po6::pathname spareshard_fn;
    e::intrusive_ptr<hyperdisk::shard> spareshard;

    {
        po6::threads::mutex::hold hold(&m_spare_shards_lock);

        if (!m_spare_shards.empty())
        {
            std::pair<po6::pathname, e::intrusive_ptr<hyperdisk::shard> > p;
            p = m_spare_shards.front();
            spareshard_fn = p.first;
            spareshard = p.second;
            m_spare_shards.pop();
        }
    }

    po6::pathname path = shard_filename(c);

    if (spareshard)
    {
        if (renameat(m_base.get(), spareshard_fn.get(),
                     m_base.get(), shard_filename(c).get()) < 0)
        {
            throw po6::error(errno);
        }

        return spareshard;
    }
    else
    {
        e::intrusive_ptr<hyperdisk::shard> newshard = hyperdisk::shard::create(m_base, path);
        return newshard;
    }
}

e::intrusive_ptr<hyperdisk::shard>
hyperdisk :: disk :: create_tmp_shard(const coordinate& c)
{
    po6::pathname spareshard_fn;
    e::intrusive_ptr<hyperdisk::shard> spareshard;

    {
        po6::threads::mutex::hold hold(&m_spare_shards_lock);

        if (!m_spare_shards.empty())
        {
            std::pair<po6::pathname, e::intrusive_ptr<hyperdisk::shard> > p;
            p = m_spare_shards.front();
            spareshard_fn = p.first;
            spareshard = p.second;
            m_spare_shards.pop();
        }
    }

    po6::pathname path = shard_tmp_filename(c);

    if (spareshard)
    {
        if (renameat(m_base.get(), spareshard_fn.get(),
                     m_base.get(), shard_filename(c).get()) < 0)
        {
            throw po6::error(errno);
        }

        return spareshard;
    }
    else
    {
        e::intrusive_ptr<hyperdisk::shard> newshard = hyperdisk::shard::create(m_base, path);
        return newshard;
    }
}

hyperdisk::returncode
hyperdisk :: disk :: drop_shard(const coordinate& c)
{
    // What would we do with the error?  It's just going to leave dirty data,
    // but if we can cleanly save state, then it doesn't matter.
    if (unlink(shard_filename(c).get()) < 0)
    {
        return DROPFAILED;
    }

    return SUCCESS;
}

hyperdisk::returncode
hyperdisk :: disk :: drop_tmp_shard(const coordinate& c)
{
    // What would we do with the error?  It's just going to leave dirty data,
    // but if we can cleanly save state, then it doesn't matter.
    if (unlink(shard_tmp_filename(c).get()) < 0)
    {
        return DROPFAILED;
    }

    return SUCCESS;
}

hyperdisk::coordinate
hyperdisk :: disk :: get_coordinate(const e::buffer& key)
{
    uint64_t key_hash = CityHash64(key);
    return coordinate(UINT32_MAX, static_cast<uint32_t>(key_hash), 0, 0);
}

hyperdisk::coordinate
hyperdisk :: disk :: get_coordinate(const e::buffer& key,
                                    const std::vector<e::buffer>& value)
{
    uint64_t key_hash = CityHash64(key);
    std::vector<uint64_t> value_hashes;
    CityHash64(value, &value_hashes);
    uint64_t value_hash = lower_interlace(value_hashes);
    return coordinate(UINT32_MAX, static_cast<uint32_t>(key_hash),
                      UINT32_MAX, static_cast<uint32_t>(value_hash));
}

hyperdisk::returncode
hyperdisk :: disk :: deal_with_full_shard(size_t shard_num)
{
    coordinate c = m_shards->get_coordinate(shard_num);
    shard* s = m_shards->get_shard(shard_num);

    if (s->stale_space() >= 30)
    {
        // Just clean up the shard.
        return clean_shard(shard_num);
    }
    else if (c.primary_mask == UINT32_MAX || c.secondary_mask == UINT32_MAX)
    {
        // XXX NOCOMMIT;
        assert(!"Not implemented");
        return SPLITFAILED;
    }
    else
    {
        // Split the shard 4-ways.
        return split_shard(shard_num);
    }
}

hyperdisk::returncode
hyperdisk :: disk :: clean_shard(size_t shard_num)
{
    coordinate c = m_shards->get_coordinate(shard_num);
    shard* s = m_shards->get_shard(shard_num);
    e::intrusive_ptr<hyperdisk::shard> newshard = create_tmp_shard(c);
    e::guard disk_guard = e::makeobjguard(*this, &hyperdisk::disk::drop_tmp_shard, c);
    s->copy_to(c, newshard);
    e::intrusive_ptr<shard_vector> newshard_vector;
    newshard_vector = m_shards->replace(shard_num, newshard);

    if (renameat(m_base.get(), shard_tmp_filename(c).get(),
                 m_base.get(), shard_filename(c).get()) < 0)
    {
        return DROPFAILED;
    }

    disk_guard.dismiss();
    po6::threads::mutex::hold hold(&m_shards_lock);
    m_shards = newshard_vector;
    return SUCCESS;
}

static int
which_to_split(uint32_t mask, const int* zeros, const int* ones)
{
    int32_t diff = INT32_MAX;
    int32_t pos = 0;
    diff = (diff < 0) ? (-diff) : diff;

    for (int i = 1; i < 32; ++i)
    {
        if (!(mask & (1 << i)))
        {
            int tmpdiff = ones[i] - zeros[i];
            tmpdiff = (tmpdiff < 0) ? (-tmpdiff) : tmpdiff;

            if (tmpdiff < diff)
            {
                pos = i;
                diff = tmpdiff;
            }
        }
    }

    return pos;
}

hyperdisk::returncode
hyperdisk :: disk :: split_shard(size_t shard_num)
{
    coordinate c = m_shards->get_coordinate(shard_num);
    shard* s = m_shards->get_shard(shard_num);
    e::intrusive_ptr<hyperdisk::shard_snapshot> snap = s->make_snapshot();

    // Find which bit of the secondary hash is the best to split over.
    int zeros[32];
    int ones[32];
    memset(zeros, 0, sizeof(zeros));
    memset(ones, 0, sizeof(ones));

    for (; snap->valid(); snap->next())
    {
        for (uint64_t i = 1, j = 0; i < UINT32_MAX; i <<= 1, ++j)
        {
            if (c.secondary_mask & i)
            {
                continue;
            }

            if (snap->secondary_hash() & i)
            {
                ++ones[j];
            }
            else
            {
                ++zeros[j];
            }
        }
    }

    int secondary_split = which_to_split(c.secondary_mask, zeros, ones);
    uint32_t secondary_bit = 1 << secondary_split;
    snap = s->make_snapshot();

    // Determine the splits for the two shards resulting from the split above.
    int zeros_lower[32];
    int zeros_upper[32];
    int ones_lower[32];
    int ones_upper[32];
    memset(zeros_lower, 0, sizeof(zeros_lower));
    memset(zeros_upper, 0, sizeof(zeros_upper));
    memset(ones_lower, 0, sizeof(ones_lower));
    memset(ones_upper, 0, sizeof(ones_upper));

    for (; snap->valid(); snap->next())
    {
        for (uint64_t i = 1, j = 0; i < UINT32_MAX; i <<= 1, ++j)
        {
            if (c.primary_mask & i)
            {
                continue;
            }

            if (snap->secondary_hash() & secondary_bit)
            {
                if (snap->primary_hash() & i)
                {
                    ++ones_upper[j];
                }
                else
                {
                    ++zeros_upper[j];
                }
            }
            else
            {
                if (snap->primary_hash() & i)
                {
                    ++ones_lower[j];
                }
                else
                {
                    ++zeros_lower[j];
                }
            }
        }
    }

    int primary_lower_split = which_to_split(c.primary_mask, zeros_lower, ones_lower);
    uint32_t primary_lower_bit = 1 << primary_lower_split;
    int primary_upper_split = which_to_split(c.primary_mask, zeros_upper, ones_upper);
    uint32_t primary_upper_bit = 1 << primary_upper_split;

    // Create four new shards, and scatter the data between them.
    coordinate zero_zero_coord(c.primary_mask | primary_lower_bit, c.primary_hash,
                               c.secondary_mask | secondary_bit, c.secondary_hash);
    coordinate zero_one_coord(c.primary_mask | primary_upper_bit, c.primary_hash,
                              c.secondary_mask | secondary_bit, c.secondary_hash | secondary_bit);
    coordinate one_zero_coord(c.primary_mask | primary_lower_bit, c.primary_hash | primary_lower_bit,
                              c.secondary_mask | secondary_bit, c.secondary_hash);
    coordinate one_one_coord(c.primary_mask | primary_upper_bit, c.primary_hash | primary_upper_bit,
                              c.secondary_mask | secondary_bit, c.secondary_hash | secondary_bit);

    try
    {
        e::intrusive_ptr<hyperdisk::shard> zero_zero = create_shard(zero_zero_coord);
        e::guard zzg = e::makeobjguard(*this, &hyperdisk::disk::drop_shard, zero_zero_coord);
        s->copy_to(zero_zero_coord, zero_zero);

        e::intrusive_ptr<hyperdisk::shard> zero_one = create_shard(zero_one_coord);
        e::guard zog = e::makeobjguard(*this, &hyperdisk::disk::drop_shard, zero_one_coord);
        s->copy_to(zero_one_coord, zero_one);

        e::intrusive_ptr<hyperdisk::shard> one_zero = create_shard(one_zero_coord);
        e::guard ozg = e::makeobjguard(*this, &hyperdisk::disk::drop_shard, one_zero_coord);
        s->copy_to(one_zero_coord, one_zero);

        e::intrusive_ptr<hyperdisk::shard> one_one = create_shard(one_one_coord);
        e::guard oog = e::makeobjguard(*this, &hyperdisk::disk::drop_shard, one_one_coord);
        s->copy_to(one_one_coord, one_one);

        e::intrusive_ptr<shard_vector> newshard_vector;
        newshard_vector = m_shards->replace(shard_num,
                                            zero_zero_coord, zero_zero,
                                            zero_one_coord, zero_one,
                                            one_zero_coord, one_zero,
                                            one_one_coord, one_one);

        {
            po6::threads::mutex::hold hold(&m_shards_lock);
            m_shards = newshard_vector;
        }

        zzg.dismiss();
        zog.dismiss();
        ozg.dismiss();
        oog.dismiss();
        drop_shard(c);
        return SUCCESS;
    }
    catch (std::exception& e)
    {
        return SPLITFAILED;
    }
}