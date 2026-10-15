"""Wrap a PPSSPP libretro savestate (u64 size + ChunkFile payload, padded) as a .ppst the app and --state load.

The libretro rollback core also saves sceNetAdhoc v9 differently from upstream's v9: in place of
adhocctlRequests it nests two sections of its own (sceNetAdhocMatchingState, sceNetAdhocRuntime). With --libretro-rb
those are replaced by an empty adhocctlRequests (fine for games not in an adhoc session). Its AudioList v3 has
decoder snapshots where upstream's v3 has g_primingFrames; with no decoders open it's only the count, and gets an
empty g_primingFrames added (with decoders it's refused). Its other changes
(MemBlockInfo v2, GPUCommon v7 + nextListID) the loader reads as they are.

usage: libretro_state_to_ppst.py in.state out.ppst [title] [--libretro-rb]"""
import struct, sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
libretro_rb = '--libretro-rb' in sys.argv
data = open(args[0], 'rb').read()
size = struct.unpack_from('<Q', data, 0)[0]
payload = data[8:8 + size]


def section_at(p, title, start=0):
    """Offset of a section header (16-byte zero-padded title + int32 version 1..63)."""
    key = title.encode()[:16].ljust(16, b'\0')
    i = start
    while True:
        i = p.find(key, i)
        if i < 0:
            return -1
        v = struct.unpack_from('<i', p, i + 16)[0]
        if 0 < v < 64:
            return i
        i += 1


if libretro_rb:
    adhoc = section_at(payload, 'sceNetAdhoc')
    nested = section_at(payload, 'sceNetAdhocMatchingState', adhoc)
    runtime = section_at(payload, 'sceNetAdhocRuntime', nested)
    after = section_at(payload, 'scePower', runtime)
    assert adhoc >= 0 and struct.unpack_from('<i', payload, adhoc + 16)[0] == 9
    assert 0 < nested - adhoc < 128 and runtime > nested and after > runtime, (adhoc, nested, runtime, after)
    # after-8: the runtime section's end marker, after-4: sceNetAdhoc's own.
    assert payload[after - 8:after] == struct.pack('<II', 0x42, 0x42)
    payload = payload[:nested] + struct.pack('<I', 0) + payload[after - 4:]
    size = len(payload)
    print(f'sceNetAdhoc: dropped {after - 4 - nested} bytes of nested sections')

    audio = section_at(payload, 'AudioList')
    assert struct.unpack_from('<i', payload, audio + 16)[0] == 3
    count, marker = struct.unpack_from('<iI', payload, audio + 20)
    assert count == 0 and marker == 0x42, 'AudioList with open decoders: not converted'
    payload = payload[:audio + 24] + struct.pack('<I', 0) + payload[audio + 24:]
    size = len(payload)

title = (args[2] if len(args) > 2 else 'libretro state').encode()[:127]
# SChunkHeader: Revision (5 = has title), Compress (0 = none), ExpectedSize, UncompressedSize, GitVersion[32]; then title[128].
header = struct.pack('<iiII32s', 5, 0, size, size, b'libretro') + title.ljust(128, b'\0')
open(args[1], 'wb').write(header + payload)
print(f'{size} bytes')
