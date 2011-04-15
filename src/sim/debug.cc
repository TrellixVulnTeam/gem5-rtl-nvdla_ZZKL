/*
 * Copyright (c) 2003-2005 The Regents of The University of Michigan
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
 * Authors: Nathan Binkert
 *          Steve Reinhardt
 */

#include <Python.h>

#include <string>
#include <vector>

#include "base/debug.hh"
#include "sim/debug.hh"
#include "sim/eventq.hh"
#include "sim/sim_events.hh"

using namespace std;

//
// Debug event: place a breakpoint on the process function and
// schedule the event to break at a particular cycle
//
struct DebugBreakEvent : public Event
{
    DebugBreakEvent();
    void process();     // process event
    virtual const char *description() const;
};

//
// constructor: schedule at specified time
//
DebugBreakEvent::DebugBreakEvent()
    : Event(Debug_Break_Pri)
{
    setFlags(AutoDelete);
}

//
// handle debug event: set debugger breakpoint on this function
//
void
DebugBreakEvent::process()
{
    Debug::breakpoint();
}


const char *
DebugBreakEvent::description() const
{
    return "debug breakpoint";
}

//
// handy function to schedule DebugBreakEvent on main event queue
// (callable from debugger)
//
void
schedBreakCycle(Tick when)
{
    mainEventQueue.schedule(new DebugBreakEvent, when);
    warn("need to stop all queues");
}

void
eventqDump()
{
    mainEventQueue.dump();
    warn("need to dump all queues");
}

void
py_interact()
{
    PyObject *globals;
    PyObject *locals;

    globals = PyEval_GetGlobals();
    Py_INCREF(globals);
    locals = PyDict_New();
    PyRun_String("import code", Py_file_input, globals, locals);
    PyRun_String("code.interact(local=globals())", Py_file_input,
                 globals, locals);
    Py_DECREF(globals);
    Py_DECREF(locals);
}

int remote_gdb_base_port = 7000;

int
getRemoteGDBPort()
{
    return remote_gdb_base_port;
}

// Set remote GDB base port.  0 means disable remote GDB.
// Callable from python.
void
setRemoteGDBPort(int port)
{
    remote_gdb_base_port = port;
}

