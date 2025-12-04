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

#define NBUCKET 13
#define BUF_PER_BUCKET 100

struct buf bufs[NBUF];

struct bucket {
  struct spinlock per_bucket_lock;

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
};

struct {
  // used only when cache miss and buffer needs to be reallocated
  struct spinlock buckets_lock;
  struct bucket bucket[NBUCKET];
} buckets;

#define INDEX(blockno) ((blockno) % NBUCKET)

void
binit(void)
{
  struct buf *b;
  struct bucket *bucket;

  initlock(&buckets.buckets_lock, "bcache.buckets_lock");

  for (int i = 0; i < NBUCKET; ++i) {
    bucket = buckets.bucket + i;
    char *lock_name = "bcache.per_bucket_lock_00";
    lock_name[23] += i / 10;
    lock_name[24] += i % 10;

    initlock(&bucket->per_bucket_lock, lock_name);

    // ! should be a cycled linked list
    bucket->head.prev = &bucket->head;
    bucket->head.next = &bucket->head;
  }

  bucket = &buckets.bucket[0];
  // Updated b's prev and next and previous head->next's prev and head->next
  for (b = bufs; b < bufs+NBUF; b++) {
    b->next = bucket->head.next;
    b->prev = &bucket->head;
    initsleeplock(&b->lock, "buffer");
    bucket->head.next->prev = b;
    bucket->head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  int index = INDEX(blockno);
  struct buf *b;
  struct bucket *bucket = &buckets.bucket[index];
  struct spinlock *lock = &bucket->per_bucket_lock;

  acquire(lock);

  // Is the block already cached?
  for(b = bucket->head.next; b != &bucket->head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  /*
    Acquire buckets_lock first before trying to perform substitution
    should avoid deadlock when two threads holding other's required lock
    acquire other's lock because the lock prevents more than one thread
    from trying to substitute.
  */
  acquire(&buckets.buckets_lock);

  uint64 lru = __UINT32_MAX__;
  struct buf *lru_buf = 0;
  // hold the source bucket lock until we find a lru-er one
  struct bucket *lru_bucket = 0;

  for (int i = 0; i < NBUCKET; ++i) {
    struct bucket *this_bucket = buckets.bucket + i;
    
    if (this_bucket != bucket)
      acquire(&this_bucket->per_bucket_lock);
    for (b = this_bucket->head.next; b != &this_bucket->head; b = b->next) {
      if (b->refcnt != 0 || b->timestamp >= lru) {
        continue;
      }

      // if the last one belongs to a different bucket
      // release its lock
      if (lru_bucket != this_bucket) {
        if (lru_bucket)
          release(&lru_bucket->per_bucket_lock);
        lru_bucket = this_bucket;
        lru_buf = b;
      }

      lru = b->timestamp;
    }

    // if we cannot find a better candidate, release the lock of current bucket
    // otherwise, keep the lock for substitution
    if (lru_bucket != this_bucket) {
      release(&this_bucket->per_bucket_lock);
    }
  }

  if (lru_buf == 0) {
    panic("bget: no buffers");
  }

  // Extract lru_buf from its previous bucket
  if (lru_buf->next == &lru_bucket->head) {
    // if it's the end of the list, we should change its prev->next to head 
    // and change head->prev to the second last one
    lru_buf->prev->next = &lru_bucket->head;
    lru_bucket->head.prev = lru_buf->prev;
  } else {
    lru_buf->next->prev = lru_buf->prev;
    lru_buf->prev->next = lru_buf->next;
  }

  // Done with source bucket, release its lock.
  // ? shall we do it when everything's done?
  release(&lru_bucket->per_bucket_lock);

  // Insert lru_buf to the front of destination bucket
  lru_buf->prev = &bucket->head;
  lru_buf->next = bucket->head.next;
  bucket->head.next->prev = lru_buf;        // change previous front's prev from head to lru_buf
  bucket->head.next = lru_buf;          // change head's next to lru_buf

  lru_buf->dev = dev;
  lru_buf->blockno = blockno;
  lru_buf->valid = 0;
  lru_buf->refcnt = 1;

  release(&buckets.buckets_lock);
  acquiresleep(&lru_buf->lock);

  // return with per_bucket_lock locked
  return lru_buf;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int blockno = b->blockno, index = INDEX(blockno);
  struct bucket *bucket = &buckets.bucket[index];
  struct spinlock *lock = &bucket->per_bucket_lock;

  acquire(lock);
  b->refcnt--;
  release(lock);
}

void
bpin(struct buf *b) {
  int blockno = b->blockno, index = INDEX(blockno);
  struct bucket *bucket = &buckets.bucket[index];
  struct spinlock *lock = &bucket->per_bucket_lock;

  acquire(lock);
  b->refcnt++;
  release(lock);
}

void
bunpin(struct buf *b) {
  int blockno = b->blockno, index = INDEX(blockno);
  struct bucket *bucket = &buckets.bucket[index];
  struct spinlock *lock = &bucket->per_bucket_lock;

  acquire(lock);
  b->refcnt--;
  release(lock);
}


