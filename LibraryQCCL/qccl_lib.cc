
// hipcc -I.. -DCOMPILE_FOR_ROCM=1 -std=c++17 --offload-arch=gfx90a test_main.cc

#include <algorithm>
#include <stdexcept>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <numeric>
#include <random>
#include <thread>
#include "qccl_lib.h"
#include "common/threading.hpp"
#include "common/example_utils.hpp"

#if 0
#define gprint(fmt, ...) printf(fmt"\n", ##__VA_ARGS__)
#else
#define gprint(fmt, ...)
#endif

// NOTE: if data size is small, maybe makes sense to use just normal load/store?
#if 0
#define LOAD(addr) __builtin_nontemporal_load(addr)
#else
#define LOAD(addr) (addr)[0]
#endif
// it seems that loading with cache and storing without it gives the best results
#if 1
#define STORE(x, addr) __builtin_nontemporal_store((x), (addr))
#else
#define STORE(x, addr) (addr)[0] = (x)
#endif

#define ATOMIC_LOAD(VAR)       __atomic_load_n((VAR),         __ATOMIC_ACQUIRE)
#define ATOMIC_STORE(DST, SRC) __atomic_store_n((DST), (SRC), __ATOMIC_RELEASE)

struct P2PWorkItem {
  uint32_t peer;      // send/recv peer
  uint32_t size;      // buffer size in bytes (limited by 4GB!)
  void **exchangeBuf; // shared buffer for exchanging pointers between GPUs:
                      // this should be set accordingly for each p2p channel (pair of GPUs)
                      // It has two entries: one for ptr exchange and one for end-of-transfer flag
  void *dataBuf;      // send/recv buffer 
};

struct WorkInfo
{
  P2PWorkItem recvItem, sendItem;
  void *targetBuf;    // target buffer address obtained from the receiver
};

static_assert(sizeof(WorkInfo) % sizeof(uint64_t) == 0, 
    "Size must be aligned by 8 bytes");

template < uint32_t BlockSz, uint32_t NumRegs >
__global__ void rcclKernel(WorkInfo *gworkInfo);

class GpuCommLib {

  static constexpr size_t s_defNumWorkItems = 8;
  static constexpr size_t s_numWorkThreads = 512;
  static constexpr size_t s_numRegsPerThread = 24;

  struct ThreadInfo {
    int gpuId;             // gpu ID assigned to this thread
    WorkInfo *workBuf;     // work buffer global memory
    void **exchangeBuf;    // shared buffer for exchanging pointers
    size_t numDevWorkItems;   // the number of workBuf items preallocated in device mem
    std::vector< WorkInfo > workItems;  // the list of current work items submitted
  };

  bool m_initialized = false;
  std::vector< ThreadInfo > m_infos;

public:
  static GpuCommLib& i() {
    static GpuCommLib obj;
    return obj;
  }

  QCCL_Result init(size_t nGpus) {
    if(m_initialized) return QCCL_Result::OK;

    m_infos.resize(nGpus);
    int i = 0;
    size_t exchangeSz = nGpus * sizeof(void *);
    for(auto& info : m_infos) {
      info.gpuId = i++;
      info.workItems.reserve(s_defNumWorkItems);
      CHK(cudaSetDevice(info.gpuId));
      int flags = //hipDeviceMallocDefault;
                  hipDeviceMallocFinegrained;
                  // hipDeviceMallocUncached;
                  // hipMallocSignalMemory;
      CHK(hipExtMallocWithFlags((void **)&info.exchangeBuf, exchangeSz*2, flags));
      CHK(cudaMemset(info.exchangeBuf, 0, exchangeSz*2));
      allocWorkBuf(&info, s_defNumWorkItems);
    }
    for(const auto& info : m_infos) {
      CHK(cudaSetDevice(info.gpuId));
      for(uint32_t j = 0; j < nGpus; j++) {
        if(info.gpuId == j)
          continue;
        int enable = -1;
        CHK(cudaDeviceCanAccessPeer(&enable, info.gpuId, j));
        if(enable == 0) {
          ThrowError<>("GPU %d is unable to access peer %d", info.gpuId, j);
        }
        CHK(cudaDeviceEnablePeerAccess(j, 0));
      }
    } // for info
    m_initialized = true;

#if 0
    int nBlocks = 0;
    CHK(hipOccupancyMaxActiveBlocksPerMultiprocessor(&nBlocks, 
      rcclKernel<s_numWorkThreads, s_numRegsPerThread>, s_numWorkThreads, 
      sizeof(WorkInfo)));
    VLOG("Max blocks per SM: " << nBlocks);
#endif
    return QCCL_Result::OK;
  }

  // function run on a thread: this ID receives from recvPeer and 
  // sends to sendPeer
  QCCL_Result enqueueSendRecv(uint32_t ID, uint32_t recvPeer, void *recvBuf,
        size_t recvSize, uint32_t sendPeer, void *sendBuf, size_t sendSize) {

    if(!m_initialized) return QCCL_Result::NotInitialized;
    if(ID >= m_infos.size()) return QCCL_Result::InvalidParams;
    auto& info = m_infos[ID];
    // NOTE: exchange pointers are always allocated on the receiver side!!
    auto& w = info.workItems.emplace_back();
    w.recvItem = { // whom we are receiving from
          .peer = recvPeer,
          .size = (uint32_t)recvSize,
          // exchange buf on the receiver side: two entries per link
          .exchangeBuf = (void **)m_infos[ID].exchangeBuf + recvPeer*2,
          .dataBuf = recvBuf,
    };
    w.sendItem = { // whom we are sending to
          .peer = sendPeer,
          .size = (uint32_t)sendSize,
          // exchange buf on the receiver side: two entries per link
          .exchangeBuf = (void **)m_infos[sendPeer].exchangeBuf + ID*2, 
          .dataBuf = sendBuf,
    };
    w.targetBuf = nullptr;
    return QCCL_Result::OK;
  }

  // execute previously enqueued send-recv tasks for this thread (one GPU)
  QCCL_Result run(uint32_t ID, cudaStream_t stream) {

    auto& info = m_infos[ID];
    CHK(cudaSetDevice(info.gpuId));

    if(info.numDevWorkItems < info.workItems.size()) {
      CHK(cudaFree(info.workBuf));
      allocWorkBuf(&info, info.numDevWorkItems * 3 / 2);
    }
    uint32_t nBlocks = info.workItems.size();
    // VLOG("Work Item size: " << sizeof(WorkInfo) << " executing with #blocks:" 
    //       << nBlocks);

    CHK(cudaMemcpyAsync(info.workBuf, info.workItems.data(), 
          sizeof(WorkInfo) * nBlocks, cudaMemcpyHostToDevice, stream));
    
    constexpr uint32_t BlockSz = s_numWorkThreads;
    rcclKernel<BlockSz, s_numRegsPerThread><<<nBlocks, BlockSz, 0, stream>>>
                                      (info.workBuf);
    info.workItems.clear();
    return QCCL_Result::OK;
  }

  ~GpuCommLib() {
    for(auto& info : m_infos) {
      (void)cudaSetDevice(info.gpuId);
      (void)cudaFree(info.workBuf);
      (void)cudaFree(info.exchangeBuf);
    }
  }
  
protected:
  // there should be a common buffer between each pair of GPUs communicated
  GpuCommLib() = default;

  QCCL_Result allocWorkBuf(ThreadInfo *pinfo, size_t num) {
    pinfo->numDevWorkItems = num;
    auto bytes = sizeof(WorkInfo) * num;
    CHK(hipExtMallocWithFlags((void **)&pinfo->workBuf, bytes, hipDeviceMallocDefault));
    return QCCL_Result::OK;
  }

}; // GpuCommLib

QCCL_Result qcclInit(uint32_t nGpus) {
  return GpuCommLib::i().init(nGpus);
}

QCCL_Result qcclSendRecv(uint32_t ID, uint32_t recvPeer, void *recvBuf,
        size_t recvSize, uint32_t sendPeer, void *sendBuf, size_t sendSize) {
  return GpuCommLib::i().enqueueSendRecv(ID, recvPeer, recvBuf,
        recvSize, sendPeer, sendBuf, sendSize);
}

QCCL_Result qcclRun(uint32_t ID, cudaStream_t stream) {
  return GpuCommLib::i().run(ID, stream);
}

//Matrix< WorkInfo > p2pSend, p2pRecv;
// ncclSend(a,b) -> set p2pSend(a,b) = sendBuf, sendSize (a sends to b) => a needs to get recv buffer from b
// ncclRecv(b,a) -> set p2pRecv(b,a) = recvBuf, recvSize (b receives from a) => b needs give its recv buffer to a

__shared__ WorkInfo s_workInfo;

// ltid is a local tid within group !!!
__forceinline__ __device__ void setupRecvPtrs(uint32_t ltid) {

  // we provide the sender our receive buffer
  if(ltid == 0) {
    auto& item = s_workInfo.recvItem;
    void *volatile *slot = item.exchangeBuf;
    *(uint32_t *)(slot + 1) = 0; // reset 'receive complete' flag

    // Wait for consumer to consume previous value before trampling it.
    while((void *)atomicAdd((uint64_t *)slot, 0) != nullptr);
    // Encode pointer by XOR'ing against some address they definitely wouldn't send
    // since we want to allow them sending us nullptr while not colliding with
    // the empty slot value.
    *slot = (void *)(reinterpret_cast<uintptr_t>(item.dataBuf) ^ 
                     reinterpret_cast<uintptr_t>(slot));
    gprint("%p Sent target buffer: %p to the sender peer %d", 
          slot, item.dataBuf, s_workInfo.sendItem.peer);
  }
}

__forceinline__ __device__ void setupSendPtrs(uint32_t ltid) {

  auto& item = s_workInfo.sendItem;
  if(ltid == 0) {
    void *volatile *slot = item.exchangeBuf;
    void *ptr;
    while (true) {
      ptr = (void *)atomicAdd((uint64_t *)slot, 0);
      if (ptr != nullptr) break;    
    }
    s_workInfo.targetBuf = (void *)(reinterpret_cast<uintptr_t>(ptr) ^ 
                                    reinterpret_cast<uintptr_t>(slot));
    *slot = nullptr;
    gprint("%p: Received target buf: %p from peer %d", 
            slot, s_workInfo.targetBuf, s_workInfo.recvItem.peer);
  }
}

template < typename Word, uint32_t BlockSz, uint32_t NumRegs, 
        bool UseOuterLoop, bool Check >
__forceinline__ __device__ 
void copyMainLoop(uint32_t ofs, const uint32_t niters, const uint32_t nwords) {

  auto srcBuf = (const Word *)s_workInfo.sendItem.dataBuf;
  auto targetBuf = (Word *)s_workInfo.targetBuf;

  Word regs[NumRegs];
  for(uint32_t s = 0; s < niters; s++) {
    auto src_ofs = ofs;
#pragma unroll
    for(uint32_t i = 0; i < NumRegs/2; i++, src_ofs += BlockSz*2) {
      if(!Check || src_ofs < nwords)
        regs[2*i] = LOAD(srcBuf + src_ofs);
      if(!Check || src_ofs + 1 < nwords)
        regs[2*i + 1] = LOAD(srcBuf + src_ofs + 1);
    }
#pragma unroll
    for(uint32_t i = 0; i < NumRegs/2; i++, ofs += BlockSz*2) {
      if(!Check || ofs < nwords)
        STORE(regs[2*i], targetBuf + ofs);
      if(!Check || ofs + 1 < nwords)
        STORE(regs[2*i + 1], targetBuf + ofs + 1);
    }
    if(!UseOuterLoop) break;
  } // for ofs
  if(s_workInfo.recvItem.peer == 0) {
    // uint32_t tid = threadIdx.x;
    // int diff = s_workInfo.sendItem.size - ofs*sizeof(Word);
    // gprint("%d: ofs: %d byteOfs: %d diff: %d", tid, ofs, ofs*sizeof(Word), 
    //       diff);
  }
}

// there maybe many work items: one for each gpu block..
template < uint32_t BlockSz, uint32_t NumRegs >
__launch_bounds__(BlockSz, 1)
__global__ void rcclKernel(WorkInfo *gworkInfo) { 

  constexpr uint32_t s_num = sizeof(WorkInfo) / sizeof(uint64_t),
            warpSize = 64;

  uint32_t tid = threadIdx.x;
  if(tid < s_num) {
    auto d = ((uint64_t *)gworkInfo)[tid];
    ((uint64_t *)&s_workInfo)[tid] = d;
  }
  __syncthreads();

  // we will use directWrite: that is, each sender writes data to receiver buffer directly
  // for that, receiver should provide sender the buffer address
  if(tid < warpSize) {
    setupRecvPtrs(tid);
  } else if(tid < warpSize*2) {
    setupSendPtrs(tid - warpSize);
  }

  __syncthreads();

  using Word = uint64_t;
  const uint32_t bytes = s_workInfo.sendItem.size, 
                 nwords = bytes / sizeof(Word),
                 niters = nwords / (BlockSz * NumRegs);
/*
Speed with 24 regs / 512 threads
Data size: 8.86 Mb; time elapsed: 0.307 ms, bandwidth: 30.245 Gb/s
Data size: 13.29 Mb; time elapsed: 0.434 ms, bandwidth: 32.139 Gb/s
Data size: 19.93 Mb; time elapsed: 0.608 ms, bandwidth: 34.401 Gb/s
Data size: 29.90 Mb; time elapsed: 0.878 ms, bandwidth: 35.701 Gb/s
Data size: 44.85 Mb; time elapsed: 1.287 ms, bandwidth: 36.554 Gb/s
Data size: 67.28 Mb; time elapsed: 1.905 ms, bandwidth: 37.038 Gb/s
Data size: 100.91 Mb; time elapsed: 2.824 ms, bandwidth: 37.475 Gb/s
Data size: 151.37 Mb; time elapsed: 4.226 ms, bandwidth: 37.562 Gb/s
Data size: 227.06 Mb; time elapsed: 6.295 ms, bandwidth: 37.820 Gb/s
Data size: 283.50 Mb; time elapsed: 8.474 ms, bandwidth: 35.082 Gb/s

Speed with 32 regs / 512 threads
Data size: 8.86 Mb; time elapsed: 0.309 ms, bandwidth: 30.089 Gb/s
Data size: 13.29 Mb; time elapsed: 0.430 ms, bandwidth: 32.391 Gb/s
Data size: 19.93 Mb; time elapsed: 0.615 ms, bandwidth: 34.005 Gb/s
Data size: 29.90 Mb; time elapsed: 0.886 ms, bandwidth: 35.406 Gb/s
Data size: 44.85 Mb; time elapsed: 1.309 ms, bandwidth: 35.930 Gb/s
Data size: 67.28 Mb; time elapsed: 1.908 ms, bandwidth: 36.981 Gb/s
Data size: 100.91 Mb; time elapsed: 2.831 ms, bandwidth: 37.374 Gb/s
Data size: 151.37 Mb; time elapsed: 4.228 ms, bandwidth: 37.544 Gb/s
Data size: 227.06 Mb; time elapsed: 6.300 ms, bandwidth: 37.793 Gb/s
Data size: 283.50 Mb; time elapsed: 8.613 ms, bandwidth: 34.515 Gb/s
*/
  copyMainLoop< Word, BlockSz, NumRegs, true, false >
                         (tid*2, niters, 0);

  if(1)
  {
    constexpr uint32_t bytesPerIter = BlockSz*NumRegs*sizeof(Word);
    const uint32_t bytesLeft = bytes - niters*bytesPerIter,
                   wordsLeft = bytesLeft / sizeof(uint32_t),
                   nwords32 = bytes / sizeof(uint32_t);

    if(wordsLeft >= bytesPerIter/2) {
      // do one full iteration with reduced size
    }

    if(tid == 0) {
      gprint("nwords: %d; bytes: %d mod16: %d niters: %d "
             "bytesPerIter: %d bytesLeft: %d wordsLeft: %d ll: %d", 
            nwords, bytes, bytes%16, niters, bytesPerIter, 
            bytesLeft, wordsLeft, nwords32);
    }
    // we are left with at most BlockSz*NumRegs*sizeof(Word)/sizeof(uint32_t)
    // 32-bit words to process: 512*16*2 = 16384 words at most
    // nbytes divisible by 4 !!
    // or if we double the number of 32-bit regs => no need for outer loop ??
    auto ofs = tid*2 + niters*bytesPerIter/sizeof(uint32_t),
         src_ofs = ofs;
    copyMainLoop< uint32_t, BlockSz, NumRegs*2, false, true >
                          (ofs, 1, nwords32);
  }
  __threadfence_system(); // TODO check if it's correct

  // NOTE: it could be that some channel is only sender or only receiver ??

  auto recvDone = (volatile uint32_t *)(s_workInfo.recvItem.exchangeBuf + 1);
  auto sendDone = (volatile uint32_t *)(s_workInfo.sendItem.exchangeBuf + 1);
  sendDone[0] = 11111;
  // __atomic_store_n(send_done, 1, __ATOMIC_SEQ_CST);

  if(tid == 0) {
    gprint("Receiver waiting peer: %d", s_workInfo.sendItem.peer);
    while(atomicAdd((uint32_t *)recvDone, 0u) != 11111u);
    gprint("Waiting done.. %d", s_workInfo.sendItem.peer);
  }
}
