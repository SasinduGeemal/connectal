#!/usr/bin/python
## Copyright (c) 2013-2014 Quanta Research Cambridge, Inc.

## Permission is hereby granted, free of charge, to any person
## obtaining a copy of this software and associated documentation
## files (the "Software"), to deal in the Software without
## restriction, including without limitation the rights to use, copy,
## modify, merge, publish, distribute, sublicense, and/or sell copies
## of the Software, and to permit persons to whom the Software is
## furnished to do so, subject to the following conditions:

## The above copyright notice and this permission notice shall be
## included in all copies or substantial portions of the Software.

## THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
## EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
## MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
## NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
## BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
## ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
## CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
## SOFTWARE.

import os, sys, shutil, string
import argparse
import util

argparser = argparse.ArgumentParser("Generate Top.bsv for an project.")
argparser.add_argument('--project-dir', help='project directory')
argparser.add_argument('-v', '--verbose', help='Display verbose information messages', action='store_true')
argparser.add_argument('-l', '--leds', help='module that exports led interface')
argparser.add_argument('-w', '--wrapper', help='exported wrapper interfaces', action='append')
argparser.add_argument('-p', '--proxy', help='exported proxy interfaces', action='append')
argparser.add_argument('-m', '--mem', help='exported memory interfaces', action='append')

noisyFlag=True

topTemplate='''
import Vector::*;
import Portal::*;
import CtrlMux::*;
import HostInterface::*;
%(generatedImport)s

typedef enum {%(enumList)s} IfcNames deriving (Eq,Bits);

module mkConnectalTop
`ifdef IMPORT_HOSTIF
       #(HostType host)
`endif
       (%(moduleParam)s);
%(portalInstantiate)s

   Vector#(%(portalCount)s,StdPortal) portals;
%(portalList)s
   let ctrl_mux <- mkSlaveMux(portals);
   interface interrupt = getInterruptVector(portals);
   interface slave = ctrl_mux;
   interface masters = %(portalMaster)s;
   interface Empty pins;
   endinterface
%(portalLeds)s
endmodule : mkConnectalTop
'''

memTemplate='''
   MMUIndicationProxy lMMUIndicationProxy <- mkMMUIndicationProxy(MMUIndicationH2S);
   MMU#(PhysAddrWidth) lMMU <- mkMMU(0, True, lMMUIndicationProxy.ifc);
   MMURequestWrapper lMMURequestWrapper <- mkMMURequestWrapper(MMURequestS2H, lMMU.request);

   MemServerIndicationProxy lMemServerIndicationProxy <- mkMemServerIndicationProxy(MemServerIndicationH2S);
   MemServer#(PhysAddrWidth,DataBusWidth,`NumberOfMasters) dma <- %(serverType)s(lMemServerIndicationProxy.ifc, %(clientList)s, cons(lMMU,nil));
   MemServerRequestWrapper lMemServerRequestWrapper <- mkMemServerRequestWrapper(MemServerRequestS2H, dma.request);
'''

def addPortal(name):
    global portalCount
    portalList.append('   portals[%(count)s] = %(name)s.portalIfc;' % {'count': portalCount, 'name': name})
    portalCount = portalCount + 1

def instMod(fmt, modname, modext):
    pmap['modname'] = modname
    pmap['modext'] = modext
    portalInstantiate.append(('   %(modname)s%(tparam)s l%(modname)s <- mk%(modname)s(' + fmt + ');') % pmap)
    instantiatedModules.append(modname)
    importfiles.append(modname)

if __name__=='__main__':
    options = argparser.parse_args()

    if options.verbose:
        noisyFlag = True
    if not options.project_dir:
        print "topgen: --project-dir option missing"
        sys.exit(1)
    project_dir = os.path.abspath(os.path.expanduser(options.project_dir))
    topFilename = project_dir + '/Top.bsv'
    if noisyFlag:
        print 'Writing Top:', topFilename
    userFiles = []
    portalInstantiate = []
    portalList = []
    portalCount = 0
    instantiatedModules = []
    importfiles = []
    portalLeds = ''
    portalMem = ''
    portalMaster = 'nil'
    moduleParam = 'StdConnectalTop#(PhysAddrWidth)'
    enumList = []
    clientList = 'l%(elementType)s'

    if options.leds:
        portalLeds = '   interface leds = l%s;' % options.leds
    for pitem in options.proxy:
        p = pitem.split(':')
        print 'PROXY', p, len(p)
        pmap = {'name': p[0], 'usermod': p[1], 'count': portalCount, 'param': '', 'tparam': ''}
        if len(p) > 2 and p[2]:
            pmap['param'] = p[2] + ', '
        addPortal('l%(name)sProxy' % pmap)
        portalInstantiate.append('   %(name)sProxy l%(name)sProxy <- mk%(name)sProxy(%(name)sH2S);' % pmap)
        instantiatedModules.append(pmap['name'] + 'Proxy')
        importfiles.append(pmap['name'])
        enumList.append(pmap['name'] + 'H2S')
        if len(p) > 3 and p[3]:
            pmap['tparam'] = '#(' + p[3] + ')'
        instMod('%(param)sl%(name)sProxy.ifc', pmap['usermod'], '')
    for pitem in options.wrapper:
        p = pitem.split(':')
        pr = p[1].split('.')
        print 'WRAPPER', p, pr, len(p)
        pmap = {'name': p[0], 'usermod': pr[0], 'userIf': p[1], 'count': portalCount, 'param': '', 'tparam': ''}
        if len(p) > 2 and p[2]:
            pmap['param'] = p[2] + ', '
        if len(p) > 3 and p[3]:
            pmap['tparam'] = '#(' + p[3] + ')'
        addPortal('l%(name)sWrapper' % pmap)
        if pmap['usermod'] not in instantiatedModules:
            portalInstantiate.append('   %(usermod)s%(tparam)s l%(usermod)s <- mk%(usermod)s(%(param)s);' % pmap)
            instantiatedModules.append(pmap['usermod'])
            importfiles.append(pmap['usermod'])
        pmap['tparam'] = ''
        portalInstantiate.append('   %(name)sWrapper l%(name)sWrapper <- mk%(name)sWrapper(%(name)sS2H, l%(userIf)s);' % pmap)
        instantiatedModules.append(pmap['name'] + 'Wrapper')
        enumList.append(pmap['name'] + 'S2H')
        importfiles.append(pmap['name'])
    if options.mem:
        print 'MEM', options.mem
        importfiles.extend(['SpecialFIFOs', 'StmtFSM', 'FIFO', 'MemTypes', 'MemServer',
            'MMU', 'ConnectalMemory', 'Leds', 'MemServerRequest',
            'MMURequest', 'MemServerIndication', 'MMUIndication'])
        for pitem in options.mem:
            p = pitem.split(':')
            ctemp = [clientList % {'elementType': pname} for pname in p[1:]]
            portalMem = portalMem + memTemplate % {'serverType': p[0], 'clientList': ','.join(ctemp)}
        moduleParam = 'ConnectalTop#(PhysAddrWidth,DataBusWidth,Empty,`NumberOfMasters)'
        portalMaster = 'dma.masters'
        addPortal('lMemServerIndicationProxy')
        enumList.append('MemServerIndicationH2S')
        addPortal('lMemServerRequestWrapper')
        enumList.append('MemServerRequestS2H')
        addPortal('lMMURequestWrapper')
        enumList.append('MMURequestS2H')
        addPortal('lMMUIndicationProxy')
        enumList.append('MMUIndicationH2S')

    topsubsts = {'enumList': ','.join(enumList),
                 'generatedImport': '\n'.join(['import %s::*;' % p for p in importfiles]),
                 'portalInstantiate' : '\n'.join(portalInstantiate) + portalMem,
                 'portalList': '\n'.join(portalList),
                 'portalCount': portalCount,
                 'portalLeds' : portalLeds,
                 'portalMem' : portalMem,
                 'portalMaster' : portalMaster,
                 'moduleParam' : moduleParam,
                 }
    print 'TOPFN', topFilename
    top = util.createDirAndOpen(topFilename, 'w')
    top.write(topTemplate % topsubsts)
    top.close()
