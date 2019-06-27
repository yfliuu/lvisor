#!/usr/bin/env python3

from kerneltest import *

class TestVMM(KernelTestCase):
    @kernel('hello32.elf')
    def test_hello32(self):
        self.assertOutput('^Hello from protected mode!$')

    @kernel('hello64.bin')
    def test_hello64(self):
        self.assertOutput('^Hello from long mode!$')

    @kernel('lv6.bin')
    def test_lv6(self):
        self.assertOutput('^\[.{12}\] hey 481$')
        self.assertOutput('^\[.{12}\] bye 451$')

    @kernel('xv6/kernelmemfs')
    def test_xv6(self):
        # boot correctly
        self.assertOutput('^cpu0: starting$')
        self.assertOutput('^init: starting sh$')

        # file system
        self.input('ls\n')
        self.assertOutput('^README')
        self.assertOutput('^console')

        # run test
        self.input('usertests\n')
        self.assertOutput('^ALL TESTS PASSED$')

if __name__ == '__main__':
    unittest.main(testRunner=KernelTestRunner)
