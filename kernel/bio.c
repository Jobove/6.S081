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

struct {
  // The lock that must be acquire before any proccess trys to evict buf.
  struct spinlock lock;
  struct buf buf[NBUF];
} bcache;

#define HNUM (13)

struct hashtable {
  struct buf head[HNUM];
  struct spinlock lock[HNUM];
} hashtable;

struct buf *
hget(uint dev, int blockno)
{
  uint i = blockno % HNUM;

  struct buf *b = &hashtable.head[i];
  while(b != 0){
    if(b->dev == dev && b->blockno == blockno){
      return b;
    }
    b = b->next;
  }

  return 0;
}

void
hinit(void)
{
  int i;
  for (i = 0; i < HNUM; ++i){
    char lockname[16];
    snprintf(lockname, 16, "bcache.bucket%d", i);

    initlock(&hashtable.lock[i], lockname);
  }
}

void
hadd(uint blockno, struct buf *b)
{
  b->blockno = blockno;
  b->refcnt = 0;
  b->valid = 0;

  uint i = blockno % HNUM;
  struct buf *tail = &hashtable.head[i];
  while(tail->next != 0)
    tail = tail->next;

  tail->next = b;
  b->prev = tail;
  b->next = 0;
}

void
binit(void)
{
  hinit();  // Initialize hashtable
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    hadd(b - bcache.buf, b);
    initsleeplock(&b->lock, "buffer");
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  uint index = blockno % HNUM;
  struct buf *b;

  // Is the block already cached?
  acquire(&hashtable.lock[index]);
  if((b = hget(dev, blockno)) != 0){
    b->refcnt++;
    release(&hashtable.lock[index]);
    acquiresleep(&b->lock);

    return b;
  }
  release(&hashtable.lock[index]);

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  int earliest = __INT32_MAX__;
  struct buf *select = 0;

  // Any process trying to evict should acquire bcache.lock first.
  // However, do not try to lock hashtable.lock[index] before here which may cause deadlock.
  // Remember to double-check if the block is already cached because of we release hashtable.lock[index].
  acquire(&bcache.lock);
  // Look for the LRU.
  // As mentioned above, to avoid deadlock, acquire hashtable.lock[] ascendingly, which means there will be time gap between previous check of whether block is already cached and the following selecting process.
  for(int i = 0; i < HNUM; ++i){
    acquire(&hashtable.lock[i]);
    struct buf *ptr = &hashtable.head[i];
    int selected = 0;
    while(ptr != 0) {
      // Continue if the current buf is being used or one of the heads.
      if(ptr->refcnt > 0 || ptr->tick >= earliest || ptr == &hashtable.head[i]){
        ptr = ptr->next;
        continue;
      }

      // If there's a better choice, release the lock of the latter one, except when there's no buf selected, or the buf is in the same bucket with the one that's being bget() or the same bucket with the last one selected.
      int relcond = select != 0 && i != index && selected == 0;
      if(relcond)
        release(&hashtable.lock[select->blockno % HNUM]);
      // Mark that indicates there's one buf being selected in this bucket.
      selected = 1;
      earliest = ptr->tick;
      select = ptr;
      ptr = ptr->next;
    }
    // Release the lock of the bucket, if there's no buf being selected or the current bucket is the one the buf being bget() should be in.
    if(!selected && i != index)
      release(&hashtable.lock[i]);
  }
  // After the loop above, both (or the only one needed) bucket(s) are locked

  // Because of the time gap mentioned above, double-check if there's existing one that's of the same blockno. If so, return.
  if((b = hget(dev, blockno)) != 0){
    b->refcnt++;
    if(select && (select->blockno % HNUM != index))
      release(&hashtable.lock[select->blockno % HNUM]);
    release(&hashtable.lock[index]);
    release(&bcache.lock);
    acquiresleep(&b->lock);

    return b;
  }
  if(!select)
    panic("bget: no buffers");

  int previ = select->blockno % HNUM;

  if(previ != index){
    struct buf *tail = &hashtable.head[index];
    // Because of hashtable.head[], there will always be a select->prev.
    select->prev->next = select->next;
    if(select->next != 0)
      select->next->prev = select->prev;

    // Release the bucket from which the buf is moved that is no longer needed.
    release(&hashtable.lock[previ]);

    while(tail->next != 0){
      tail = tail->next;
    }
    tail->next = select;
    select->prev = tail;
    select->next = 0;

    select->dev = dev;
    select->blockno = blockno;
    select->valid = 0;
    select->refcnt = 1;
  } else {
    select->dev = dev;
    select->blockno = blockno;
    select->valid = 0;
    select->refcnt = 1;
  }

  release(&hashtable.lock[index]);
  release(&bcache.lock);
  acquiresleep(&select->lock);

  return select;
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

  uint i = b->blockno % HNUM;

  acquire(&hashtable.lock[i]);
  b->refcnt--;
  release(&hashtable.lock[i]);

  releasesleep(&b->lock);
}

void
bpin(struct buf *b) {
  uint i = b->blockno % HNUM;
  acquire(&hashtable.lock[i]);
  b->refcnt++;
  release(&hashtable.lock[i]);
}

void
bunpin(struct buf *b) {
  uint i = b->blockno % HNUM;
  acquire(&hashtable.lock[i]);
  b->refcnt--;
  release(&hashtable.lock[i]);
}


