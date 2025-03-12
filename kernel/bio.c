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

#define BNUM (13)

struct {
  struct spinlock lock;
  struct buf buf[NBUF * BNUM];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

#define HNUM (13)

struct hashtable {
  struct buf head[HNUM];
  struct spinlock lock[HNUM];
} hashtable;

struct buf *
hget(int blockno)
{
  uint i = blockno % HNUM;

  struct buf *b = &hashtable.head[i];
  while(b != 0){
    if(b->blockno == blockno){
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
    char lockname[9];
    snprintf(lockname, 9, "bcache%d", i);

    initlock(&hashtable.lock[i], lockname);
  }
}

void
binit(void)
{
  hinit();  // Initialize hashtable
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    // b->next = bcache.head.next;
    // b->prev = &bcache.head;
    b->blockno = -1;
    b->refcnt = 0;
    initsleeplock(&b->lock, "buffer");
    // bcache.head.next->prev = b;
    // bcache.head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  // Is the block already cached?
  acquire(&hashtable.lock[blockno % HNUM]);
  if((b = hget(blockno)) != 0){
    b->refcnt++;
    release(&hashtable.lock[blockno % HNUM]);
    acquiresleep(&b->lock);

    return b;
  }
  release(&hashtable.lock[blockno % HNUM]);

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  int i, earliest = __INT32_MAX__;
  struct buf *select = 0;

  acquire(&bcache.lock);  // Prevent other proc from evicting.
  // Look for the LRU.
  for(i = 0; i < NBUF; ++i){
    if(bcache.buf[i].refcnt)
      continue;

    if(bcache.buf[i].tick >= earliest)
      continue;

    earliest = bcache.buf[i].tick;
    select = &bcache.buf[i];
  }
  if(!select)
    panic("bget: no buffers");
  if(select->blockno == -1){
    int index = blockno % HNUM;

    acquire(&hashtable.lock[index]);
    struct buf *node = &hashtable.head[index];
    while(node->next)
      node = node->next;

    node->next = select;
    node->next->blockno = blockno;
    select->prev = node;
    select->next = 0;
    release(&hashtable.lock[index]);

    select->dev = dev;
    select->blockno = blockno;
    select->valid = 0;
    select->refcnt = 1;

    release(&bcache.lock);
    acquiresleep(&select->lock);
    return select;
  }

  int previ = select->blockno % HNUM, newi = select->blockno % HNUM;

  if(previ != newi){
    // Only if one proc owns bcache.lock will it try to lock more locks so no dead lock is probable.
    acquire(&hashtable.lock[previ]);
    acquire(&hashtable.lock[newi]);

    struct buf *node = hget(blockno), *tail = &hashtable.head[newi];
    if(node->prev != &hashtable.head[previ])
      node->prev->next = node->next;
    if(node->next != 0)
      node->next->prev = node->prev;

    while(tail->next != 0){
      tail = tail->next;
    }
    tail->next = node;
    node->prev = tail;
    node->next = 0;

    select->dev = dev;
    select->blockno = blockno;
    select->valid = 0;
    select->refcnt = 1;

    release(&hashtable.lock[previ]);
    release(&hashtable.lock[newi]);
  } else {
    acquire(&hashtable.lock[previ]);

    select->dev = dev;
    select->blockno = blockno;
    select->valid = 0;
    select->refcnt = 1;

    release(&hashtable.lock[previ]);
  }

  release(&bcache.lock);
  acquiresleep(&select->lock);

  if(select->refcnt > 100)
    printf("debug");

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
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = hashtable.head[i].next;
    b->prev = &hashtable.head[i];
    hashtable.head[i].next->prev = b;
    hashtable.head[i].next = b;
  }

  release(&hashtable.lock[i]);

  releasesleep(&b->lock);

  // acquire(&bcache.lock);
  // b->refcnt--;
  // if (b->refcnt == 0) {
  //   // no one is waiting for it.
  //   b->next->prev = b->prev;
  //   b->prev->next = b->next;
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }

  // release(&bcache.lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  if(b->refcnt > 100)
    printf("debug");
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


