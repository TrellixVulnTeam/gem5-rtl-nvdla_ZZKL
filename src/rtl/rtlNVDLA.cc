/*
 * Copyright (c) 2022 Guillem Lopez Paradis
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Guillem Lopez Paradis
 */

#include "rtl/rtlNVDLA.hh"

namespace gem5
{


rtlNVDLA::rtlNVDLA(const rtlNVDLAParams &params) :
    rtlObject(params),
    cpuPort(params.name + ".cpu_side", this),
    memPort(params.name + ".mem_side", this),
    sramPort(params.name + ".sram_port", this, true),
    dramPort(params.name + ".dram_port", this, false),
    bytesToRead(0),
    bytesReaded(0),
    blocked(false),
    max_req_inflight(params.maxReq),
    timingMode(params.enableTimingAXI),
    id_nvdla(params.id_nvdla),
    baseAddrDRAM(params.base_addr_dram),
    baseAddrSRAM(params.base_addr_sram),
    waiting_for_gem5_mem(0)
{

    initNVDLA();
    startMemRegion = 0xC0000000;
    cyclesNVDLA = 0;
    std::cout << std::hex << "NVDLA " << id_nvdla
              << " Base Addr DRAM: " << baseAddrDRAM
              << " Base Addr SRAM: " << baseAddrSRAM << std::endl;
    // Clear input
    memset(&input,0,sizeof(inputNVDLA));
}


rtlNVDLA::~rtlNVDLA() {
    delete wr;
}

Port &
rtlNVDLA::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "mem_side") {
        return memPort;
    } else if (if_name == "cpu_side") {
        return cpuPort;
    } else if (if_name == "sram_port") {
        return sramPort;
    } else if (if_name == "dram_port") {
        return dramPort;
    } else {
        panic_if(true, "Asking to rtlNVDLA for a port different\
                        than cpu or mem");
        return ClockedObject::getPort(if_name, idx);
    }
}

bool
rtlNVDLA::handleRequest(PacketPtr pkt)
{
    // Here we have just received the start rtlNVDLA function
    // we check if there is an outstanding call
    // otherwise start getting the whole trace
    if (blocked) {
        // There is currently an outstanding request. Stall.
        return false;
    }

    blocked = true;

    DPRINTF(rtlNVDLA, "Got request for size: %d, addr: %#x\n",
                        pkt->getSize(),
                        pkt->req->getVaddr());

    bytesToRead = pkt->getSize();

    ptrTrace = (char *) malloc(bytesToRead);

    startTranslate(pkt->req->getVaddr(), 0);

    return true;
}

void
rtlNVDLA::initNVDLA() {
    // Wrapper
    wr = new Wrapper_nvdla(traceEnable, "trace.vcd",max_req_inflight);
    // wrapper trace from nvidia
    trace = new TraceLoaderGem5(wr->csb, wr->axi_dbb, wr->axi_cvsram);
}

void
rtlNVDLA::initRTLModel() {

}
void
rtlNVDLA::endRTLModel() {

}

void
rtlNVDLA::loadTraceNVDLA(char *ptr) {
    // load the trace into the queues
    trace->load(ptr);

    startBaseTrace = trace->getBaseAddr();

    DPRINTF(rtlNVDLA,
            "Base Addr: %#x \n",
            trace->getBaseAddr());
    // reset NVDLA
    wr->init();
    // init some variable before exec of trace
    quiesc_timer = 200;
    waiting = 0;
    // we run at 1GHz (current system runs at 2Ghz)
    schedule(tickEvent,nextCycle()+1);
}

void
rtlNVDLA::processOutput(outputNVDLA out) {

    if (out.read_valid) {
        while (!out.read_buffer.empty()) {
            read_req_entry_t aux = out.read_buffer.front();
            printf("read req: %08x\n", aux.read_addr);
            // std::cout << std::hex << "read req: " \
            // << aux.read_addr << std::endl;
            readAXIVariable(aux.read_addr,
                            aux.read_sram,
                            aux.read_timing,
                            aux.read_bytes);
            out.read_buffer.pop();
        }
    }
    if (out.write_valid) {
        while (!out.write_buffer.empty()) {
            write_req_entry_t aux = out.write_buffer.front();
            printf("write req: addr %08x, data %02x\n", \
            aux.write_addr, aux.write_data);
            // std::cout << std::hex << "write req: addr " <<\
            // aux.write_addr << ", data " << aux.write_data << std::endl;
            writeAXI(aux.write_addr,
                     aux.write_data,
                     aux.write_sram,
                     aux.write_timing);
            out.write_buffer.pop();
        }
    }
}

void
rtlNVDLA::runIterationNVDLA() {
    wr->clearOutput();

    int extevent;

    if (!waiting_for_gem5_mem)
        extevent = wr->csb->eval(waiting);
    else
        extevent = 0;

    if (extevent == TraceLoaderGem5::TRACE_AXIEVENT || waiting_for_gem5_mem)
        trace->axievent(&waiting_for_gem5_mem);
    else if (extevent == TraceLoaderGem5::TRACE_WFI) {
        waiting = 1;
        printf("(%lu) waiting for interrupt...\n", wr->tickcount);
    }

    if (waiting && wr->dla->dla_intr) {
        printf("(%lu) interrupt!\n", wr->tickcount);
        waiting = 0;
    }


    if (timingMode) {
        wr->axi_dbb->eval_timing();
        wr->axi_cvsram->eval_timing();
    } else {
        wr->axi_dbb->eval_ram();
        wr->axi_cvsram->eval_ram();
    }


    outputNVDLA output = wr->tick(input);
    processOutput(output);
}

void
rtlNVDLA::tick() {
    DPRINTF(rtlNVDLADebug, "Tick NVDLA \n");
    // if we are still running trace
    // runIteration
    // schedule new iteration
    if (!wr->csb->done() || (quiesc_timer--) || waiting_for_gem5_mem) {
        // Update stats
        stats.nvdla_avgReqCVSRAM.sample(wr->axi_cvsram->getRequestsOnFlight());
        stats.nvdla_avgReqDBBIF.sample(wr->axi_dbb->getRequestsOnFlight());
        stats.nvdla_cycles++;
        cyclesNVDLA++;
        runIterationNVDLA();
        schedule(tickEvent,nextCycle()+1);
    }
    else {
        // we have finished running the trace
        printf("done at %lu ticks\n", wr->tickcount);

        if (!trace->test_passed()) {
            printf("*** FAIL: test failed due to output mismatch\n");

        } else if (!wr->csb->test_passed()) {
            printf("*** FAIL: test failed due to CSB read mismatch\n");
        } else {
            std::cout << "NVDLA " << id_nvdla;
            printf(" *** PASS\n");
        }

        // we send a null packet telling we have finished
        RequestPtr req = std::make_shared<Request>(id_nvdla, 1,
                                               Request::UNCACHEABLE, 0);
        PacketPtr packet = nullptr;
        // we create the real packet, write request
        packet = Packet::createRead(req);
        packet->allocate();
        packet->makeResponse();
        cpuPort.sendPacket(packet);
    }
    // check DRAM Ports
    dramPort.tick();
    sramPort.tick();
}

void
rtlNVDLA::tmpRunTraceNVDLA() {

    printf("Running trace...\n");

    while (!wr->csb->done() || (quiesc_timer--)) {
        runIterationNVDLA();
    }

    printf("done at %lu ticks\n", wr->tickcount);

    if (!trace->test_passed()) {
        printf("*** FAIL: test failed due to output mismatch\n");
        return ;
    }

    if (!wr->csb->test_passed()) {
        printf("*** FAIL: test failed due to CSB read mismatch\n");
        return ;
    }

    printf("*** PASS\n");
}

void
rtlNVDLA::runTraceNVDLA(char *ptr) {
    // Run whole trace for debug purposes
    wr = new Wrapper_nvdla(traceEnable, "trace.vcd",max_req_inflight);
    wr->disableTracing();

    TraceLoaderGem5 *trace;
    trace = new TraceLoaderGem5(wr->csb, wr->axi_dbb, wr->axi_cvsram);
    trace->load(ptr);
    wr->init();

    printf("running trace...\n");
    uint32_t quiesc_timer = 200;
    int waiting = 0;
    while (!wr->csb->done() || (quiesc_timer--)) {
        int extevent;

        extevent = wr->csb->eval(waiting);

        if (extevent == TraceLoaderGem5::TRACE_AXIEVENT)
            trace->axievent(&waiting_for_gem5_mem);
        else if (extevent == TraceLoaderGem5::TRACE_WFI) {
            waiting = 1;
            printf("(%lu) waiting for interrupt...\n", wr->tickcount);
        }

        if (waiting && wr->dla->dla_intr) {
            printf("(%lu) interrupt!\n", wr->tickcount);
            waiting = 0;
        }

        wr->axi_dbb->eval_atomic();
        if (wr->axi_cvsram)
            wr->axi_cvsram->eval_atomic();

        wr->tick();
    }

    printf("done at %lu ticks\n", wr->tickcount);

    if (!trace->test_passed()) {
        printf("*** FAIL: test failed due to output mismatch\n");
        return ;
    }

    if (!wr->csb->test_passed()) {
        printf("*** FAIL: test failed due to CSB read mismatch\n");
        return ;
    }

    printf("*** PASS\n");
}

bool
rtlNVDLA::handleResponse(PacketPtr pkt)
{
    if (pkt->hasData()){

        char *data_ptr = pkt->getPtr<char>();

        int maxRead = ((bytesToRead - bytesReaded) > 64) ?
                         64 : (bytesToRead - bytesReaded);

        for (int i=0;i<maxRead;i++) {
            ptrTrace[bytesReaded+i] = data_ptr[i];
            //DPRINTF(rtlNVDLA, "Got response for addr %x %02x\n",
            //                        pkt->getAddr()+(i),
            //                        data_ptr[i]);
        }

        bytesReaded += 64;

        if (bytesReaded < bytesToRead) {
            startTranslate(pkt->req->getVaddr()+64, 0);
        } else {
            //for (int i=0;i<bytesToRead;i++) {
            //    DPRINTF(rtlNVDLA, "Trace: %02x\n",
            //                        ptrTrace[i]);
            //}

            bytesReaded = 0;
            bytesToRead = 0;

            // Load the trace and reset NVDLA
            loadTraceNVDLA(ptrTrace);
        }
    }
    else {
        // Strange situation, report!
        DPRINTF(rtlNVDLA, "Got response for addr %#x no data\n",
                            pkt->getAddr());
    }


    // The packet is now done. We're about to put it in the port, no need for
    // this object to continue to stall.
    // We need to free the resource before sending the packet in case the CPU
    // tries to send another request immediately (e.g., in the same callchain).
    blocked = false;

    return true;
}

bool
rtlNVDLA::handleResponseNVDLA(PacketPtr pkt, bool sram)
{
    if (pkt->hasData()){
        if (pkt->isRead()){
            // Get data from gem5 memory system
            // and set it to AXI
            DPRINTF(rtlNVDLADebug,
                    "Handling response for data read Timing\n");
            // Get the data ptr and sent it
            const uint8_t* dataPtr = pkt->getConstPtr<uint8_t>();
            uint32_t addr_nvdla = getAddrNVDLA(pkt->getAddr(),sram);
            if (sram) {
                // SRAM
                wr->axi_cvsram->inflight_resp(addr_nvdla,dataPtr);
            }
            else {
                // DBBIF
                wr->axi_dbb->inflight_resp(addr_nvdla,dataPtr);
            }
        } else {
            // this is somehow odd, report!
            DPRINTF(rtlNVDLA, "Got response for addr %#x no read\n",
                    pkt->getAddr());
        }
    }
    else {
         DPRINTF(rtlNVDLA, "Got response for addr %#x no data\n",
         pkt->getAddr());
    }

    return true;
}

void
rtlNVDLA::handleFunctional(PacketPtr pkt)
{
    // Just pass this on to the memory side to handle for now.
    memPort.sendFunctional(pkt);
}

AddrRangeList
rtlNVDLA::getAddrRanges() const
{
    DPRINTF(rtlNVDLA, "Sending new ranges\n");
    // Just use the same ranges as whatever is on the memory side.
    return memPort.getAddrRanges();
}

void
rtlNVDLA::sendRangeChange()
{
    cpuPort.sendRangeChange();
}

void
rtlNVDLA::finishTranslation(WholeTranslationState *state) {

    DPRINTF(rtlNVDLA, "Finishing translation\n");

    RequestPtr req = state->mainReq;

    if (req->hasPaddr()) {
        DPRINTF(rtlNVDLA,
                "Finished translation step: Got request for addr %#x %#x\n",
        state->mainReq->getVaddr(),state->mainReq->getPaddr());

    } else {
        // we end up in a situation we shouldn't be
        DPRINTF(rtlNVDLA,
                "Finished translation without physical addr\n");
    }

    PacketPtr new_pkt = new Packet(req, MemCmd::ReadReq, 64);

    if (memPort.blockedPacket != nullptr) {
        DPRINTF(rtlNVDLA, "Packet lost\n");
    }
    else {
        new_pkt->allocate();
        memPort.sendPacket(new_pkt);
    }
}

// DRAM PORT
void
rtlNVDLA::MemNVDLAPort::sendPacket(PacketPtr pkt, bool timing)
{
    if (timing) {
        DPRINTF(rtlNVDLA, "Add Mem Req pending %#x size: %d timing s: %d\n",
            pkt->getAddr(), pkt->getSize(), pending_req.size());
        // we add as a pending request, we deal later
        pending_req.push(pkt);
    }
    else {
        DPRINTF(rtlNVDLA, "Send Mem Req to DRAM %#x size: %d functional\n",
            pkt->getAddr(), pkt->getSize());
        // send Atomic
        sendAtomic(pkt);
        // Update all the pointers
        recentData32 = *pkt->getConstPtr<uint32_t>();
        recentData = *pkt->getConstPtr<uint8_t>();
        recentDataptr = pkt->getConstPtr<uint8_t>();
    }
}

void
rtlNVDLA::MemNVDLAPort::recvRangeChange()
{
    owner->sendRangeChange();
}

bool
rtlNVDLA::MemNVDLAPort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(rtlNVDLA, "Got response SRAM: %d\n", sram);
    return owner->handleResponseNVDLA(pkt,sram);
}

void
rtlNVDLA::MemNVDLAPort::recvReqRetry()
{
    // we check we have pending packets
    assert(blockedRetry);
    // Grab the blocked packet.
    PacketPtr pkt = pending_req.front();
    bool sent = sendTimingReq(pkt);
    // if not sent put it in the queue
    if (sent) {
        pending_req.pop();
        blockedRetry = false;
    }
    // else
    // we do nothing
    // we failed sending the packet we wait
}

// In this function we send the packets
void
rtlNVDLA::MemNVDLAPort::tick()
{
    // we check we have pending packets
    if (!blockedRetry and !pending_req.empty()) {
        PacketPtr pkt = pending_req.front();
        bool sent = sendTimingReq(pkt);
        // if not sent, put it in the queue
        if (sent) {
            pending_req.pop();
        } else {
            blockedRetry = true;
        }
    }
}

uint32_t
rtlNVDLA::getRealAddr(uint32_t addr, bool sram) {

    uint32_t real_addr;

    if (sram) {
        // Base addr is 0x5000_0000
        real_addr = (addr - 0x50000000) + baseAddrSRAM;
    } else {
        // Base addr is 0x8000_0000
        real_addr = (addr - 0x80000000) + baseAddrDRAM;
    }
    return real_addr;
}

uint32_t
rtlNVDLA::getAddrNVDLA(uint32_t addr, bool sram) {

    uint32_t real_addr;

    if (sram) {
        // Base addr is 0x5000_0000
        //baseAddrDRAM
        real_addr = (addr - baseAddrSRAM) + 0x50000000;
        //real_addr = (addr - 0xC0000000) + 0x50000000;
    } else {
        // Base addr is 0x8000_0000
        // 0xA0000000
        real_addr = (addr - baseAddrDRAM) + 0x80000000;
    }
    return real_addr;
}


uint8_t
rtlNVDLA::readAXI(uint32_t addr, bool sram, bool timing) {
    // Update stats
    stats.nvdla_reads++;
    // calclulate the addr
    uint32_t real_addr = getRealAddr(addr,sram);
    // Create packet
    RequestPtr req = std::make_shared<Request>(real_addr, 1,
                                               Request::UNCACHEABLE, 0);
    PacketPtr packet = nullptr;
    // we create the real packet, write request
    packet = Packet::createRead(req);
    packet->allocate();
    // send the packet in timing?
    if (sram) {
        sramPort.sendPacket(packet, timing);
        return sramPort.recentData;
    } else {
        dramPort.sendPacket(packet, timing);
        return dramPort.recentData;
    }
    return 1;
}

uint32_t
rtlNVDLA::readAXI32(uint32_t addr, bool sram, bool timing) {

    // Update stats
    stats.nvdla_reads++;

    uint32_t real_addr = getRealAddr(addr,sram);

    RequestPtr req = std::make_shared<Request>(real_addr, 4,
                                               Request::UNCACHEABLE, 0);
    PacketPtr packet = nullptr;
    // we create the real packet, write request
    packet = Packet::createRead(req);
    packet->allocate();
    // send the packet in timing?
    if (sram) {
        sramPort.sendPacket(packet, timing);
        return sramPort.recentData32;
    } else {
        dramPort.sendPacket(packet, timing);
        return dramPort.recentData32;
    }
    return 1;
}

const uint8_t *
rtlNVDLA::readAXIVariable(uint32_t addr, bool sram,
                             bool timing,
                             unsigned int size) {
    // Update stats
    stats.nvdla_reads++;

    uint32_t real_addr = getRealAddr(addr,sram);

    DPRINTF(rtlNVDLA,
            "Read AXI Variable addr: %#x, real_addr %#x\n",
            addr, real_addr);

    RequestPtr req = std::make_shared<Request>(real_addr, size,
                                               Request::UNCACHEABLE, 0);
    PacketPtr packet = nullptr;
    // we create the real packet, write request
    packet = Packet::createRead(req);
    packet->allocate();
    // send the packet in timing?
    if (sram) {
        sramPort.sendPacket(packet, timing);
        return sramPort.recentDataptr;
    } else {
        dramPort.sendPacket(packet, timing);
        return dramPort.recentDataptr;
    }
    return nullptr;
}

void
rtlNVDLA::writeAXI(uint32_t addr, uint8_t data, bool sram, bool timing) {
    // Update stats
    stats.nvdla_writes++;

    uint32_t real_addr = getRealAddr(addr,sram);

    DPRINTF(rtlNVDLA,
            "Write AXI Variable addr: %#x, real_addr %#x, data_to_write 0x%02x\n",
            addr, real_addr, data);
    //Request(Addr paddr, unsigned size, Flags flags, MasterID mid)
    // addr is the physical addr
    // size is one byte
    // flags is physical (vaddr is also the physical one)
    RequestPtr req = std::make_shared<Request>(real_addr, 1,
                                               Request::UNCACHEABLE, 0);
    PacketPtr packet = nullptr;
    // we create the real packet, write request
    packet = Packet::createWrite(req);
    // always in Little Endian
    PacketDataPtr dataAux = new uint8_t[1];
    dataAux[0] = data;

    packet->dataDynamic(dataAux);
    // send the packet in timing?
    if (sram) {
        sramPort.sendPacket(packet, timing);
    } else {
        dramPort.sendPacket(packet, timing);
    }

}

void
rtlNVDLA::regStats()
{
    // If you don't do this you get errors about uninitialized stats.
    ClockedObject::regStats();

    using namespace statistics;

    stats.nvdla_cycles
        .name(name() + ".nvdla_cycles")
        .desc("Number of Cycles to run the trace");

    stats.nvdla_reads
        .name(name() + ".nvdla_reads")
        .desc("Number of reads performed");

    stats.nvdla_writes
        .name(name() + ".nvdla_writes")
        .desc("Number of writes performed");
    stats.nvdla_avgReqCVSRAM
        .init(256)
        .name(name() + ".nvdla_avgReqCVSRAM")
        .desc("Histogram Requests onflight CVSRAM")
        .flags(pdf);

    stats.nvdla_avgReqDBBIF
        .init(256)
        .name(name() + ".nvdla_avgReqDBBIF")
        .desc("Histogram Requests onflight DBBIF")
        .flags(pdf);

}

} //End namespace gem5
