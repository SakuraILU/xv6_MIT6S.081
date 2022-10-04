// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct
{
  struct spinlock eviction_lock; // lock for buffer eviction
  struct buf buf[NBUF];          // buffers

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf heads[NBUCKET];             // heads of each bucket (the heads of bilinked lists)
  struct spinlock bucket_locks[NBUCKET]; // locks for each bucket
} bcache;

void binit(void)
{
  // init the lock for buffer eviction
  initlock(&bcache.eviction_lock, "bcache");

  // init buffer buckets and related small locks
  char lock_name[] = "bucket_0";
  for (int i = 0; i < NBUCKET; i++)
  {
    lock_name[7] = ('0' + i);
    initlock(&bcache.bucket_locks[i], lock_name);
    bcache.heads[i].prev = &bcache.heads[i];
    bcache.heads[i].next = &bcache.heads[i];
  }

  // allocate buffers evenly to each bucket according to its position in buffer array
  for (int i = 0; i < NBUF; i++)
  {
    uint64 bucket_idx = i % NBUCKET;
    // append the buffer to the bilinked list
    bcache.buf[i].next = bcache.heads[bucket_idx].next;
    bcache.buf[i].prev = &bcache.heads[bucket_idx];
    initsleeplock(&bcache.buf[i].lock, "buffer");
    bcache.heads[bucket_idx].next->prev = bcache.buf + i;
    bcache.heads[bucket_idx].next = bcache.buf + i;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.

/// @brief
// Look through buffer cache for the block on device which is numerically marked as (dev, blockno).
// If not found, allocate a buffer.
// In either case, return locked buffer.
// Also, only bread and bwrite try to get the buf, this buffer will be used,
// thus the last_use attribute in this buf will be updated.
/// @param dev serial number of the device
/// @param blockno serial number of the block in the device
/// @return the buffer used to store the data in the block
static struct buf *bget(uint dev, uint blockno)
{
  struct buf *b;
  uint64 bucket_idx = blockno % NBUCKET;

  acquire(&bcache.bucket_locks[bucket_idx]);

  // Is the block already cached?
  for (b = bcache.heads[bucket_idx].next; b != &bcache.heads[bucket_idx]; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      b->last_use = ticks;
      release(&bcache.bucket_locks[bucket_idx]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // no free buffer block, need to find an LRU unsed buffer,
  // eviction it out of its original bucket and move it to current bucket to buffer (dev, blockno)'s data
  release(&bcache.bucket_locks[bucket_idx]);
  acquire(&bcache.eviction_lock);

  // Is the block already cached?
  // To eviction LRU unused buffer, first give up current lock, so other process can visit this bucket, may also want to use (dev, blockno).
  // thus many processes waiting eviction_lock may want the same (dev, blockno),
  // thus before eviction...first check wether this (dev, blockno) already is obtained by other processes before
  acquire(&bcache.bucket_locks[bucket_idx]);
  for (b = bcache.heads[bucket_idx].next; b != &bcache.heads[bucket_idx]; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      b->last_use = ticks;
      release(&bcache.bucket_locks[bucket_idx]);
      release(&bcache.eviction_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket_locks[bucket_idx]);

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  // Unused means unlinked, b->refcnt is zero.
  uint lru = -1; // MAX value of uint
  int found_new = 0;
  int found_bucket_idx = -1;
  struct buf *found = 0;
  // traverse all the buckets to found the lru unused buffer
  for (int i = 0; i < NBUCKET; ++i)
  {
    acquire(&bcache.bucket_locks[i]);
    found_new = 0;
    for (b = bcache.heads[i].next; b != &bcache.heads[i]; b = b->next)
    {
      // b->refcnt == 0: unused
      // b->last_use < lru: the last_used time is smaller than the buffer currently founded, found a smaller one
      if (b->refcnt == 0 && (b->last_use < lru))
      {
        // release old buf's bucket lock, if the same as current bucket searched, no need to realse
        if (found_bucket_idx != -1 && found_bucket_idx != i)
          release(&bcache.bucket_locks[found_bucket_idx]);
        found_new = 1;
        found_bucket_idx = i;
        found = b;
        lru = b->last_use;
      }
    }
    // if not found a new buffer in this bucket, realse it's lock
    if (!found_new)
      release(&bcache.bucket_locks[i]);
  }

  if (found)
  {
    // set the buffer's attr, especally the new (dev, blockno)
    found->dev = dev;
    found->blockno = blockno;
    found->valid = 0;
    found->refcnt = 1;
    found->last_use = ticks; // update last used time
    // if the lru unused buffer found is not in the current bucekt, move it to current bucket
    if (found_bucket_idx != bucket_idx)
    {
      // move out of it's original bucket
      found->prev->next = found->next;
      found->next->prev = found->prev;
      release(&bcache.bucket_locks[found_bucket_idx]);
      // append it to the current bucket
      acquire(&bcache.bucket_locks[bucket_idx]);
      found->prev = &bcache.heads[bucket_idx];
      found->next = bcache.heads[bucket_idx].next;
      bcache.heads[bucket_idx].next->prev = found;
      bcache.heads[bucket_idx].next = found;
      release(&bcache.bucket_locks[bucket_idx]);
    }
    else
      release(&bcache.bucket_locks[found_bucket_idx]);

    // eviction process is over, unlock eviction_lock
    release(&bcache.eviction_lock);
    acquiresleep(&found->lock);
    return found;
  }

  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno)
{
  // printf("bread\n");
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid)
  {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // unsed: refcnt == 0, so just decrease refcnt is enough
  bunpin(b);
}

void bpin(struct buf *b)
{
  // blockno will never change once it is used (refcnt != 0), only change in bget()
  // so, don't need b->lock here...
  uint64 bucket_idx = b->blockno % NBUCKET;
  acquire(&bcache.bucket_locks[bucket_idx]);
  b->refcnt++;
  release(&bcache.bucket_locks[bucket_idx]);
}

void bunpin(struct buf *b)
{
  // blockno will never change once it is used (refcnt != 0), only change in bget()
  // so, don't need b->lock here...
  uint64 bucket_idx = b->blockno % NBUCKET;
  acquire(&bcache.bucket_locks[bucket_idx]);
  b->refcnt--;
  release(&bcache.bucket_locks[bucket_idx]);
}
