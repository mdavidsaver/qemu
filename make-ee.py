#!/usr/bin/env python3

# image for BNL PSC
#  24AA025E48T-I/OT 2Kb (256 byte) I2C eeprom

from socket import inet_aton

def getargs():
    from argparse import ArgumentParser
    P = ArgumentParser()
    def mac(s):
        return bytes([int(o,16) for o in s.split(':',5)])
    P.add_argument('--mac', type=mac, default=mac('01:02:03:04:05:06'))
    P.add_argument('--ip', type=inet_aton, default=inet_aton('0.0.0.0'))
    P.add_argument('out')
    return P

def main(args):
    with open(args.out, 'wb') as F:
        F.write(bytes([0]*256))

        F.seek(95)
        F.write(args.ip)

        F.seek(0xFA)
        F.write(args.mac)

        assert F.seek(0, 2)==0x100

if __name__=='__main__':
    main(getargs().parse_args())
