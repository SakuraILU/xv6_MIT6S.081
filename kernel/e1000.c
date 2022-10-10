#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++)
  {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64)tx_ring;
  if (sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;

  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++)
  {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64)rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64)rx_ring;
  if (sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA + 1] = 0x5634 | (1 << 31);
  // multicast table
  for (int i = 0; i < 4096 / 32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |                 // enable
                     E1000_TCTL_PSP |                // pad short packets
                     (0x10 << E1000_TCTL_CT_SHIFT) | // collision stuff
                     (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8 << 10) | (6 << 20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN |      // enable receiver
                     E1000_RCTL_BAM |     // enable broadcast
                     E1000_RCTL_SZ_2048 | // 2048-byte rx buffers
                     E1000_RCTL_SECRC;    // strip CRC

  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0;       // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0;       // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

/// @brief transmit data to e1000
/// @param m mbuf carrying the ethernet frame data
/// @return 0 on success while -1 on error
int e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //

  // many process may transmit through e1000 concurrently, so hold the lock when modifies related data
  acquire(&e1000_lock);
  // E100_TDT is the idx of the next transmission position
  uint32 tx_tail = regs[E1000_TDT];
  struct tx_desc *desc = &tx_ring[tx_tail];
  // E1000_TXD_STAT_DD (descriptor down, os can write data now):
  // means ok for transmission, no data waiting to transmited here
  // otherwise the old mbuf is not transmited successfully yet, return
  if (!(desc->status & E1000_TXD_STAT_DD))
  {
    release(&e1000_lock);
    return -1;
  }
  // record mbuf in mbufs array, remove the old mbuf if exist
  if (tx_mbufs[tx_tail])
  {
    mbuffree(tx_mbufs[tx_tail]);
    tx_mbufs[tx_tail] = 0;
  }
  tx_mbufs[tx_tail] = m;

  // set the descriptor for e1000, head addr and length of this mbuf
  desc->addr = (uint64)m->head;
  desc->length = m->len;
  // E1000_TXD_CMD_EOP: packet is end, all data is written
  // E1000_TXD_CMD_RS: need e1000's response, set E1000_TXD_STAT_DD if the mbuf in the descriptor is transmited over
  desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;

  // move E1000_TDT to the next pos
  regs[E1000_TDT] = (++regs[E1000_TDT]) % TX_RING_SIZE;
  release(&e1000_lock);
  return 0;
}

/// @brief
// Receives data from e1000. This function is called in usertrap(),
// when e1000 send an interrput, the calling stack is usetrap()->devintr()->e1000_recv()
static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //

  // the speed of e1000 and the os is different, so there may several data arrives, read them all out to sockets
  // Note: when e1000 send an interrput, e1000_recv will be called once, after received data,
  //       tell e1000 can send another interrput by setting regs[E1000_ICR] = 0xfffffff,
  //       thus no concurrency problem here, don't use lock here
  //       but i don't know why if i use lock here, cpu will occur lock re-entry bug.....anyway, don't use lock!
  while (1)
  {
    // E1000_RDT points the last position where mbuf is readed, to read new data, move one
    uint32 rx_tail = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
    struct rx_desc *desc = &rx_ring[rx_tail]; // get the e1000's descriptor
    // E1000_RXD_STAT_DD (descriptor down):
    // means it's ok for reading, the mbuf is recevied successfully,
    // otherwise no new data prepared, return
    if (!(desc->status & E1000_RXD_STAT_DD))
    {
      return;
    }

    // get the mbuf and pass upwards through the network protocal stack (xv6 only support UDP)
    rx_mbufs[rx_tail]->len = desc->length;
    net_rx(rx_mbufs[rx_tail]);

    // malloc a new buf here
    rx_mbufs[rx_tail] = mbufalloc(0);
    desc->addr = (uint64)rx_mbufs[rx_tail]->head;
    desc->length = 0;
    desc->status = 0; // clear old status

    // finished reading, set E1000_RDT to the new pos
    regs[E1000_RDT] = rx_tail;
  }
}

/// @brief handles e1000's interrput
void e1000_intr(void)
{
  // receive data from e1000
  e1000_recv();

  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xfffffff;
}
