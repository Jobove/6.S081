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
  // struct buf head;
} bcache;

// struct rwlock {
//   struct spinlock lock;
//   uint reader;
//   uint write;
// };

// void
// rwinit(struct rwlock *l, const char *name)
// {
//   l->reader = 0;
//   initlock(&l->lock, name);
// }

// uint
// rwread(struct rwlock *l)
// {
//   acquire(&l->lock);
//   if(l->write) {
//     release(&l->lock);
//     return 0;
//   }
//   l->reader++;
//   release(&l->lock);
//   return 1;
// }

// void
// rwunread(struct rwlock *l)
// {
//   acquire(&l->lock);
//   l->reader--;
//   release(&l->lock);
// }

// uint
// rwwrite(struct rwlock *l)
// {
//   acquire(&l->lock);
//   if(l->reader || l->write) {
//     release(&l->lock);
//     return 0;
//   }
//   l->write = 1;
//   release(&l->lock);
// }

// uint
// rwunwrite(struct rwlock *l)
// {
//   acquire(&l->lock);
//   l->write = 0;
//   release(&l->lock);
// }

#define HNUM (31)

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
    snprintf(lockname, 16, "bcache%d", i);

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
    // b->blockno = __INT32_MAX__;
    // b->refcnt = 0;
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

  acquire(&bcache.lock);  // Prevent other proc from evicting.
  // Look for the LRU.
  for(int i = 0; i < HNUM; ++i){
    acquire(&hashtable.lock[i]);
    struct buf *ptr = &hashtable.head[i];
    int selected = 0;
    while(ptr != 0) {
      if(ptr->refcnt > 0 || ptr->tick >= earliest || ptr == &hashtable.head[i]){
        ptr = ptr->next;
        continue;
      }

      int relcond = select != 0 && i != index && selected == 0;
      if(relcond)
        release(&hashtable.lock[select->blockno % HNUM]);
      selected = 1;
      earliest = ptr->tick;
      select = ptr;
      ptr = ptr->next;
    }
    if(!selected && i != index)
      release(&hashtable.lock[i]);
  }
/*   for (int i = 0; i < NBUF; ++i){
    uint idx = bcache.buf[i].blockno % HNUM, isequal = (idx == index);
    int relcond = select != 0 && ((select->blockno % HNUM) != idx);
    if(!isequal){
      acquire(&hashtable.lock[idx]);
    }
    if(bcache.buf[i].refcnt > 0 || bcache.buf[i].tick >= earliest){
      if(relcond)
        release(&hashtable.lock[idx]);
      continue;
    }

    if(relcond){
      release(&hashtable.lock[idx]);
      acquire(&hashtable.lock[select->blockno % HNUM]);
    }
    earliest = bcache.buf[i].tick;
    select = &bcache.buf[i];
  } */
/*   for(i = 0; i < NBUF; ++i){
    if(bcache.buf[i].refcnt > 0)
      continue;

    if(bcache.buf[i].tick >= earliest)
      continue;

    earliest = bcache.buf[i].tick;
    select = &bcache.buf[i];
  } */
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


/*   // Selected block has never been used. Add it to the hash table.
  if(select->blockno == __INT32_MAX__){
    struct buf *node = &hashtable.head[index];
    while(node->next)
      node = node->next;

    node->next = select;
    node->next->blockno = blockno;
    select->prev = node;
    select->next = 0;

    select->dev = dev;
    select->blockno = blockno;
    select->valid = 0;
    select->refcnt = 1;

    for(int i = 0; i < HNUM; ++i)
      release(&hashtable.lock[i]);
    // release(&hashtable.lock[index]);  // Lock for bucket shouldn't be released until any modification is done, not even before dev/blockno/valid/refcnt is modified.
    release(&bcache.lock);
    acquiresleep(&select->lock);
    return select;
  } */

  int previ = select->blockno % HNUM;

  if(previ != index){
    // Only if one proc owns bcache.lock will it try to lock more locks so no dead lock is probable.
    // acquire(&hashtable.lock[previ]);

    struct buf *tail = &hashtable.head[index];     // node should always equal to select but it's unnecessary so I delete it.
    // if(node->prev != &hashtable.head[previ])   // select should always modify his prev->next.
    select->prev->next = select->next;
    if(select->next != 0)
      select->next->prev = select->prev;
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

  // for(int i = 0; i < HNUM; ++i)
  //   release(&hashtable.lock[i]);
  // release(&hashtable.lock[index]);  // Maybe this lock should always be locked.
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


