/* nvdla.cpp
 * Driver for Verilator testbench
 * NVDLA Open Source Project
 *
 * Copyright (c) 2017 NVIDIA Corporation.  Licensed under the NVDLA Open
 * Hardware License.  For more information, see the "LICENSE" file that came
 * with this distribution.
 * 
 * Slightly changed to be adapted for gem5 requirements
 * 
 * Guillem Lopez Paradis
 */



#include "axiResponder.hh"

AXIResponder::AXIResponder(struct connections _dla,
                                   Wrapper_nvdla *_wrapper,
                                   const char *_name,
                                   bool sram_,
                                   const unsigned int maxReq) {
    dla = _dla;

    wrapper = _wrapper;

    *dla.aw_awready = 1;
    *dla.w_wready = 1;
    *dla.b_bvalid = 0;
    *dla.ar_arready = 1;
    *dla.r_rvalid = 0;

    name = _name;

    sram = sram_;

    max_req_inflight = (maxReq<240) ? maxReq:240;

    // add some latency...
    for (int i = 0; i < AXI_R_LATENCY; i++) {
        axi_r_txn txn;

        txn.rvalid = 0;
        txn.rvalid = 0;
        txn.rid = 0;
        txn.rlast = 0;
        for (int i = 0; i < AXI_WIDTH / 32; i++) {
                txn.rdata[i] = 0xAAAAAAAA;
        }

        r0_fifo.push(txn);
    }
}

uint8_t
AXIResponder::read_ram(uint32_t addr) {
    ram[addr / AXI_BLOCK_SIZE].resize(AXI_BLOCK_SIZE, 0);
    return ram[addr / AXI_BLOCK_SIZE][addr % AXI_BLOCK_SIZE];
}

void
AXIResponder::write_ram(uint32_t addr, uint8_t data) {
    ram[addr / AXI_BLOCK_SIZE].resize(AXI_BLOCK_SIZE, 0);
    ram[addr / AXI_BLOCK_SIZE][addr % AXI_BLOCK_SIZE] = data;
}

void
AXIResponder::write(uint32_t addr, uint8_t data,bool timing) {
    // always write to fake ram
    write_ram(addr,data);
    // we access gem5 memory
    wrapper->addWriteReq(sram,timing,addr,data);
}

void
AXIResponder::eval_ram() {
    /* write request */
    if (*dla.aw_awvalid && *dla.aw_awready) {
        #ifdef PRINT_DEBUG
            printf("(%lu) %s: write request from dla, addr %08lx id %d\n",
                    wrapper->tickcount, name, *dla.aw_awaddr, *dla.aw_awid);
        #endif
        axi_aw_txn txn;

        txn.awid = *dla.aw_awid;
        txn.awaddr = *dla.aw_awaddr & ~(uint64_t)(AXI_WIDTH / 8 - 1);
        txn.awlen = *dla.aw_awlen;
        aw_fifo.push(txn);

        *dla.aw_awready = 0;
    } else
        *dla.aw_awready = 1;

    /* write data */
    if (*dla.w_wvalid) {
        #ifdef PRINT_DEBUG
            printf("(%lu) %s: write data from dla (%08x %08x...)\n",
                wrapper->tickcount, name, dla.w_wdata[0], dla.w_wdata[1]);
        #endif
        axi_w_txn txn;

        for (int i = 0; i < AXI_WIDTH / 32; i++)
            txn.wdata[i] = dla.w_wdata[i];
        txn.wstrb = *dla.w_wstrb;
        txn.wlast = *dla.w_wlast;
        w_fifo.push(txn);
    }

    /* read request */
    if (*dla.ar_arvalid && *dla.ar_arready) {
        uint64_t addr = *dla.ar_araddr & ~(uint64_t)(AXI_WIDTH / 8 - 1);
        uint8_t len = *dla.ar_arlen;

        #ifdef PRINT_DEBUG
            printf("(%lu) %s: %s %08lx burst %d id %d\n",
                    wrapper->tickcount, name,
                   "read request from dla, addr",
                   *dla.ar_araddr,
                   *dla.ar_arlen, *dla.ar_arid);
        #endif

        do {
            axi_r_txn txn;

            txn.rvalid = 1;
            txn.rlast = len == 0;
            txn.rid = *dla.ar_arid;

            for (int i = 0; i < AXI_WIDTH / 32; i++) {
                uint32_t da = read_ram(addr + i * 4) +
                                (read_ram(addr + i * 4 + 1) << 8) +
                                (read_ram(addr + i * 4 + 2) << 16) +
                                (read_ram(addr + i * 4 + 3) << 24);
                txn.rdata[i] = da;
            }

            r_fifo.push(txn);

            addr += AXI_WIDTH / 8;
        } while (len--);

        axi_r_txn txn;

        txn.rvalid = 0;
        txn.rid = 0;
        txn.rlast = 0;
        for (int i = 0; i < AXI_WIDTH / 32; i++) {
            txn.rdata[i] = 0x55555555;
        }

        for (int i = 0; i < AXI_R_DELAY; i++)
            r_fifo.push(txn);

        *dla.ar_arready = 0;
    } else
        *dla.ar_arready = 1;

    /* now handle the write FIFOs ... */
    if (!aw_fifo.empty() && !w_fifo.empty()) {
        axi_aw_txn &awtxn = aw_fifo.front();
        axi_w_txn &wtxn = w_fifo.front();

        if (wtxn.wlast != (awtxn.awlen == 0)) {
            printf("(%lu) %s: wlast / awlen mismatch\n",
                   wrapper->tickcount, name);
            abort();
        }

        for (int i = 0; i < AXI_WIDTH / 8; i++) {
            if (!((wtxn.wstrb >> i) & 1))
                continue;

            write_ram(awtxn.awaddr + i,
                      (wtxn.wdata[i / 4] >> ((i % 4) * 8)) & 0xFF);
        }


        if (wtxn.wlast) {
            #ifdef PRINT_DEBUG
                printf("(%lu) %s: write, last tick\n", wrapper->tickcount, name);
            #endif
            aw_fifo.pop();

            axi_b_txn btxn;
            btxn.bid = awtxn.awid;
            b_fifo.push(btxn);
        } else {
            #ifdef PRINT_DEBUG
                printf("(%lu) %s: write, ticks remaining\n",
                    wrapper->tickcount, name);
            #endif
            awtxn.awlen--;
            awtxn.awaddr += AXI_WIDTH / 8;
        }

        w_fifo.pop();
    }

    /* read response */
    if (!r_fifo.empty()) {
        axi_r_txn &txn = r_fifo.front();

        r0_fifo.push(txn);
        r_fifo.pop();
    } else {
        axi_r_txn txn;

        txn.rvalid = 0;
        txn.rid = 0;
        txn.rlast = 0;
        for (int i = 0; i < AXI_WIDTH / 32; i++) {
            txn.rdata[i] = 0xAAAAAAAA;
        }

        r0_fifo.push(txn);
    }

    *dla.r_rvalid = 0;
    if (*dla.r_rready && !r0_fifo.empty()) {
        axi_r_txn &txn = r0_fifo.front();

        *dla.r_rvalid = txn.rvalid;
        *dla.r_rid = txn.rid;
        *dla.r_rlast = txn.rlast;
        for (int i = 0; i < AXI_WIDTH / 32; i++) {
            dla.r_rdata[i] = txn.rdata[i];
        }
        #ifdef PRINT_DEBUG
            if (txn.rvalid) {
                printf("(%lu) %s: read push: id %d, da %08x %08x %08x %08x\n",
                        wrapper->tickcount, name,
                        txn.rid, txn.rdata[0], txn.rdata[1],
                        txn.rdata[2], txn.rdata[3]);
            }
        #endif

        r0_fifo.pop();
    }

    /* write response */
    *dla.b_bvalid = 0;
    if (*dla.b_bready && !b_fifo.empty()) {
        *dla.b_bvalid = 1;

        axi_b_txn &txn = b_fifo.front();
        *dla.b_bid = txn.bid;
        b_fifo.pop();
    }
}

void
AXIResponder::eval_atomic() {
    /* write request */
    if (*dla.aw_awvalid && *dla.aw_awready) {
        #ifdef PRINT_DEBUG
            printf("(%lu) %s: write request from dla, addr %08lx id %d\n",
                wrapper->tickcount,
                name,
                *dla.aw_awaddr,
                *dla.aw_awid);
        #endif

        axi_aw_txn txn;

        txn.awid = *dla.aw_awid;
        txn.awaddr = *dla.aw_awaddr & ~(uint64_t)(AXI_WIDTH / 8 - 1);
        txn.awlen = *dla.aw_awlen;
        aw_fifo.push(txn);

        *dla.aw_awready = 0;
    } else
        *dla.aw_awready = 1;

    /* write data */
    if (*dla.w_wvalid) {
        #ifdef PRINT_DEBUG
            printf("(%lu) %s: write data from dla (%08x %08x...)\n",
                wrapper->tickcount,
                name,
                dla.w_wdata[0],
                dla.w_wdata[1]);
        #endif

        axi_w_txn txn;

        for (int i = 0; i < AXI_WIDTH / 32; i++)
            txn.wdata[i] = dla.w_wdata[i];
        txn.wstrb = *dla.w_wstrb;
        txn.wlast = *dla.w_wlast;
        w_fifo.push(txn);
    }

    /* read request */
    if (*dla.ar_arvalid && *dla.ar_arready) {
        uint64_t addr = *dla.ar_araddr & ~(uint64_t)(AXI_WIDTH / 8 - 1);
        uint8_t len = *dla.ar_arlen;
        #ifdef PRINT_DEBUG
            printf("(%lu) %s: read request from dla, addr %08lx burst %d id %d\n",
                wrapper->tickcount,
                name,
                *dla.ar_araddr,
                *dla.ar_arlen,
                *dla.ar_arid);
        #endif

        do {
            axi_r_txn txn;

            txn.rvalid = 1;
            txn.rlast = len == 0;
            txn.rid = *dla.ar_arid;

            /*for (int i = 0; i < AXI_WIDTH / 32; i++) {
                uint32_t da = read_ram(addr + i * 4) +
                             (read_ram(addr + i * 4 + 1) << 8) +
                             (read_ram(addr + i * 4 + 2) << 16) +
                             (read_ram(addr + i * 4 + 3) << 24);
                txn.rdata[i] = da;
            }*/
            const uint8_t* dataPtr = read_variable(addr, false, 512/8);
            inflight_resp_atomic(addr,dataPtr,&txn);

            r_fifo.push(txn);

            addr += AXI_WIDTH / 8;
        } while (len--);

        axi_r_txn txn;

        txn.rvalid = 0;
        txn.rid = 0;
        txn.rlast = 0;
        for (int i = 0; i < AXI_WIDTH / 32; i++) {
            txn.rdata[i] = 0x55555555;
        }

        for (int i = 0; i < AXI_R_DELAY; i++)
            r_fifo.push(txn);

        *dla.ar_arready = 0;
    } else
        *dla.ar_arready = 1;

    /* now handle the write FIFOs ... */
    if (!aw_fifo.empty() && !w_fifo.empty()) {
        axi_aw_txn &awtxn = aw_fifo.front();
        axi_w_txn &wtxn = w_fifo.front();

        if (wtxn.wlast != (awtxn.awlen == 0)) {
            printf("(%lu) %s: wlast / awlen mismatch\n",
                   wrapper->tickcount, name);
            abort();
        }

        for (int i = 0; i < AXI_WIDTH / 8; i++) {
            if (!((wtxn.wstrb >> i) & 1))
                continue;

            write(awtxn.awaddr + i,
                 (wtxn.wdata[i / 4] >> ((i % 4) * 8)) & 0xFF,
                 false);
        }


        if (wtxn.wlast) {
            #ifdef PRINT_DEBUG
                printf("(%lu) %s: write, last tick\n", wrapper->tickcount, name);
            #endif
            aw_fifo.pop();

            axi_b_txn btxn;
            btxn.bid = awtxn.awid;
            b_fifo.push(btxn);
        } else {
            #ifdef PRINT_DEBUG
                printf("(%lu) %s: write, ticks remaining\n",
                       wrapper->tickcount, name);
            #endif

            awtxn.awlen--;
            awtxn.awaddr += AXI_WIDTH / 8;
        }

        w_fifo.pop();
    }

    /* read response */
    if (!r_fifo.empty()) {
        axi_r_txn &txn = r_fifo.front();

        r0_fifo.push(txn);
        r_fifo.pop();
    } else {
        axi_r_txn txn;

        txn.rvalid = 0;
        txn.rid = 0;
        txn.rlast = 0;
        for (int i = 0; i < AXI_WIDTH / 32; i++) {
            txn.rdata[i] = 0xAAAAAAAA;
        }

        r0_fifo.push(txn);
    }

    *dla.r_rvalid = 0;
    if (*dla.r_rready && !r0_fifo.empty()) {
        axi_r_txn &txn = r0_fifo.front();

        *dla.r_rvalid = txn.rvalid;
        *dla.r_rid = txn.rid;
        *dla.r_rlast = txn.rlast;
        for (int i = 0; i < AXI_WIDTH / 32; i++) {
            dla.r_rdata[i] = txn.rdata[i];
        }
        #ifdef PRINT_DEBUG
            if (txn.rvalid) {
                printf("(%lu) %s: read push: id %d, da %08x %08x %08x %08x\n",
                    wrapper->tickcount, name, txn.rid, txn.rdata[0],
                    txn.rdata[1], txn.rdata[2], txn.rdata[3]);
            }
        #endif

        r0_fifo.pop();
    }

    /* write response */
    *dla.b_bvalid = 0;
    if (*dla.b_bready && !b_fifo.empty()) {
        *dla.b_bvalid = 1;

        axi_b_txn &txn = b_fifo.front();
        *dla.b_bid = txn.bid;
        b_fifo.pop();
    }
}

void
AXIResponder::eval_timing() {
    /* write request */
    if (*dla.aw_awvalid && *dla.aw_awready) {
        #ifdef PRINT_DEBUG
            printf("(%lu) %s: write request from dla, addr %08lx id %d\n",
                wrapper->tickcount,
                name,
                *dla.aw_awaddr,
                *dla.aw_awid);
        #endif

        axi_aw_txn txn;

        txn.awid = *dla.aw_awid;
        txn.awaddr = *dla.aw_awaddr & ~(uint64_t)(AXI_WIDTH / 8 - 1);
        txn.awlen = *dla.aw_awlen;
        aw_fifo.push(txn);

        *dla.aw_awready = 0;
    } else
        *dla.aw_awready = 1;

    /* write data */
    if (*dla.w_wvalid) {
        #ifdef PRINT_DEBUG
            printf("(%lu) %s: write data from dla (%08x %08x...)\n",
                    wrapper->tickcount,
                    name,
                    dla.w_wdata[0],
                    dla.w_wdata[1]);
        #endif

        axi_w_txn txn;

        for (int i = 0; i < AXI_WIDTH / 32; i++)
            txn.wdata[i] = dla.w_wdata[i];
        txn.wstrb = *dla.w_wstrb;
        txn.wlast = *dla.w_wlast;
        w_fifo.push(txn);
    }

    /* read request */
    if (*dla.ar_arvalid && *dla.ar_arready) {
        uint64_t addr = *dla.ar_araddr & ~(uint64_t)(AXI_WIDTH / 8 - 1);
        uint8_t len = *dla.ar_arlen;
        uint8_t i = 0;
        
        #ifdef PRINT_DEBUG
            printf("(%lu) %s: read request from dla, addr %08lx burst %d id %d\n",
                wrapper->tickcount,
                name,
                *dla.ar_araddr,
                *dla.ar_arlen,
                *dla.ar_arid);
        #endif

        do {
            axi_r_txn txn;

            // write to the txn
            txn.rvalid = 0;
            txn.rlast = len == 0;
            txn.rid = *dla.ar_arid;
            // put txn to the map
            inflight_req[addr].push_back(txn);
            // put in req the txn
            inflight_req_order.push(addr);

            read_variable(addr, true, 512/8);

            addr += AXI_WIDTH / 8;
            i++;
        } while (len--);

        // next cycle we are not ready
        *dla.ar_arready = 0;
    } else {
        #ifdef PRINT_DEBUG
            if (inflight_req_order.size() > 0) {
                printf("(%lu) %s: Remaining %d\n",
                        wrapper->tickcount, name,
                        inflight_req_order.size());
            }
        #endif

        unsigned int addr_front = inflight_req_order.front();
        // if burst
        if (inflight_req[addr_front].front().rvalid) {
            // push the front one
            r_fifo.push(inflight_req[addr_front].front());
            // remove the front
            inflight_req[addr_front].pop_front();
            // check if empty to remove entry from map
            if (inflight_req[addr_front].empty()) {
                inflight_req.erase(addr_front);
            }
            // delete in the queue order
            inflight_req_order.pop();
            *dla.ar_arready = 0;
        }
        else if (inflight_req_order.size() <= max_req_inflight) {
            *dla.ar_arready = 1;
        }
        else {
            *dla.ar_arready = 0;
        }
    }

    /* now handle the write FIFOs ... */
    if (!aw_fifo.empty() && !w_fifo.empty()) {
        axi_aw_txn &awtxn = aw_fifo.front();
        axi_w_txn &wtxn = w_fifo.front();

        if (wtxn.wlast != (awtxn.awlen == 0)) {
            printf("(%lu) %s: wlast / awlen mismatch\n",
                   wrapper->tickcount, name);
            abort();
        }

        for (int i = 0; i < AXI_WIDTH / 8; i++) {
            if (!((wtxn.wstrb >> i) & 1))
                continue;

            write(awtxn.awaddr + i,
                 (wtxn.wdata[i / 4] >> ((i % 4) * 8)) & 0xFF,
                 false);
        }


        if (wtxn.wlast) {
            #ifdef PRINT_DEBUG
                printf("(%lu) %s: write, last tick\n", wrapper->tickcount, name);
            #endif
            aw_fifo.pop();

            axi_b_txn btxn;
            btxn.bid = awtxn.awid;
            b_fifo.push(btxn);
        } else {
            #ifdef PRINT_DEBUG
                printf("(%lu) %s: write, ticks remaining\n",
                   wrapper->tickcount, name);
            #endif

            awtxn.awlen--;
            awtxn.awaddr += AXI_WIDTH / 8;
        }

        w_fifo.pop();
    }

    /* read response */
    if (!r_fifo.empty()) {
        axi_r_txn &txn = r_fifo.front();

        r0_fifo.push(txn);
        r_fifo.pop();
    } else {
        axi_r_txn txn;

        txn.rvalid = 0;
        txn.rid = 0;
        txn.rlast = 0;
        for (int i = 0; i < AXI_WIDTH / 32; i++) {
            txn.rdata[i] = 0xAAAAAAAA;
        }

        r0_fifo.push(txn);
    }

    *dla.r_rvalid = 0;
    if (*dla.r_rready && !r0_fifo.empty()) {
        axi_r_txn &txn = r0_fifo.front();

        *dla.r_rvalid = txn.rvalid;
        *dla.r_rid = txn.rid;
        *dla.r_rlast = txn.rlast;
        for (int i = 0; i < AXI_WIDTH / 32; i++) {
            dla.r_rdata[i] = txn.rdata[i];
        }
        #ifdef PRINT_DEBUG
            if (txn.rvalid) {
                printf("(%lu) %s: read push: id %d, da %08x %08x %08x %08x\n",
                    wrapper->tickcount, name, txn.rid, txn.rdata[0],
                    txn.rdata[1], txn.rdata[2], txn.rdata[3]);
            }
        #endif

        r0_fifo.pop();
    }

    /* write response */
    *dla.b_bvalid = 0;
    if (*dla.b_bready && !b_fifo.empty()) {
        *dla.b_bvalid = 1;

        axi_b_txn &txn = b_fifo.front();
        *dla.b_bid = txn.bid;
        b_fifo.pop();
    }
}

void
AXIResponder::inflight_resp(uint32_t addr, const uint8_t* data) {
    #ifdef PRINT_DEBUG
        printf("(%lu) %s: Inflight Resp Timing: addr %08x \n",
                wrapper->tickcount, name, addr);
    #endif
    uint32_t * ptr = (uint32_t*) data;
    std::list<axi_r_txn>::iterator it = inflight_req[addr].begin();;
    // Get the correct ptr
    while (it != inflight_req[addr].end() and
           it->rvalid) {
               it++;
    }
    assert(it != inflight_req[addr].end());
    for (int i = 0; i < AXI_WIDTH / 32; i++) {
        it->rdata[i] = ptr[i];
        /*printf("Expecting: %0x Got %0x%0x%0x%0x", 
                ptr[i],
                read_ram(addr+i+3),
                read_ram(addr+i+2),
                read_ram(addr+i+1),
                read_ram(addr+i+0));*/
    }
    it->rvalid = 1;
    #ifdef PRINT_DEBUG
        printf("Remaining %d\n", inflight_req_order.size());
        printf("(%lu) %s: Inflight Resp Timing Finished: addr %08lx \n",
                wrapper->tickcount, name, addr);
    #endif
}

void
AXIResponder::emptyInflight() {
    printf("(%lu) %s: Empty Inflight\n",
            wrapper->tickcount, name);

    /*unsigned int addr_order = inflight_req_order.front();
    if (inflight_req[addr_order].rvalid) {
        while (inflight_req[addr_order].rvalid) {
            r_fifo.push(inflight_req[addr_order]);
            inflight_req.erase(addr_order);
            // delete in the queue order
            inflight_req_order.pop();
            // get new order
            addr_order = inflight_req_order.front();
        }
        *dla.ar_arready = 0;
    }*/
}

void
AXIResponder::inflight_resp_atomic(uint32_t addr,
                                       const uint8_t* data,
                                       axi_r_txn *txn) {
    //printf("(%lu) %s: Inflight Resp Atomic: addr %08lx \n",
    //        wrapper->tickcount, name, addr);
    uint32_t * ptr = (uint32_t*) data;
    for (int i = 0; i < AXI_WIDTH / 32; i++) {
        txn->rdata[i] = ptr[i];
    }
    txn->rvalid = 1;
}

const uint8_t*
AXIResponder::read_variable(uint32_t addr, bool timing,
                                unsigned int bytes) {
    //printf("name: %s %d\n", name, strcmp(name,"DBB"));
    wrapper->addReadReq(sram,timing,addr,bytes);
    return 0;//readGem5;
}

// Reads 1 value
uint8_t
AXIResponder::read(uint32_t addr) {
    ram[addr / AXI_BLOCK_SIZE].resize(AXI_BLOCK_SIZE, 0);

    wrapper->addReadReq(sram,false,addr,1);

    //printf("Compare reading gem5ram: %08lx, ram: %08lx \n",
    //        readGem5, readRam);
    return 0;//readGem5;
}

uint32_t
AXIResponder::getRequestsOnFlight() {
    return inflight_req_order.size();
}
