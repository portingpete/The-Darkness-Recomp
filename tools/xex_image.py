"""Read the supported retail XEX directly, using Windows' built-in AES provider."""
import ctypes
from pathlib import Path
import struct

RETAIL_KEY = bytes.fromhex('20b185a59d28fdc340583fbb0896bf91')
MAX_IMAGE = 64 * 1024 * 1024


def decrypt(data: bytes, key: bytes) -> bytes:
    if not data or len(data) % 16:
        raise ValueError('Invalid encrypted XEX length')
    cng = ctypes.WinDLL('bcrypt')
    handle = ctypes.c_void_p
    ulong = ctypes.c_ulong
    cng.BCryptOpenAlgorithmProvider.argtypes = [ctypes.POINTER(handle), ctypes.c_wchar_p, ctypes.c_wchar_p, ulong]
    cng.BCryptSetProperty.argtypes = [handle, ctypes.c_wchar_p, ctypes.c_void_p, ulong, ulong]
    cng.BCryptGenerateSymmetricKey.argtypes = [handle, ctypes.POINTER(handle), ctypes.c_void_p, ulong, ctypes.c_void_p, ulong, ulong]
    cng.BCryptDecrypt.argtypes = [handle, ctypes.c_void_p, ulong, ctypes.c_void_p, ctypes.c_void_p, ulong, ctypes.c_void_p, ulong, ctypes.POINTER(ulong), ulong]
    cng.BCryptDestroyKey.argtypes = [handle]
    cng.BCryptCloseAlgorithmProvider.argtypes = [handle, ulong]
    algorithm, symmetric = handle(), handle()

    def check(status):
        if status < 0:
            raise RuntimeError('Windows AES decoding failed')

    try:
        check(cng.BCryptOpenAlgorithmProvider(ctypes.byref(algorithm), 'AES', None, 0))
        mode = ctypes.create_unicode_buffer('ChainingModeCBC')
        check(cng.BCryptSetProperty(algorithm, 'ChainingMode', mode, ctypes.sizeof(mode), 0))
        secret = ctypes.create_string_buffer(key)
        check(cng.BCryptGenerateSymmetricKey(algorithm, ctypes.byref(symmetric), None, 0, secret, len(key), 0))
        buffer = ctypes.create_string_buffer(data, len(data))
        iv = ctypes.create_string_buffer(16)
        written = ulong()
        check(cng.BCryptDecrypt(symmetric, buffer, len(data), None, iv, 16, buffer, len(data), ctypes.byref(written), 0))
        if written.value != len(data):
            raise ValueError('Incomplete AES output')
        return buffer.raw
    finally:
        if symmetric.value:
            cng.BCryptDestroyKey(symmetric)
        if algorithm.value:
            cng.BCryptCloseAlgorithmProvider(algorithm, 0)


def decode_xex(data: bytes) -> tuple[bytes, bytes]:
    def word(offset):
        if offset < 0 or offset + 4 > len(data):
            raise ValueError('Truncated XEX field')
        return struct.unpack_from('>I', data, offset)[0]

    if not 24 <= len(data) <= MAX_IMAGE or data[:4] != b'XEX2':
        raise ValueError('Invalid XEX2 file')
    header_size, security, count = word(8), word(16), word(20)
    if not 24 <= header_size <= min(len(data), 0x100000):
        raise ValueError('Invalid XEX header size')
    if not 24 <= security <= header_size - 0x180 or count > (header_size - 24) // 8:
        raise ValueError('Invalid XEX header layout')
    image_size = word(security + 4)
    if not 0 < image_size <= MAX_IMAGE or word(security + 0x110) != 0x82000000:
        raise ValueError('Unsupported XEX image layout')
    formats = [word(28 + i * 8) for i in range(count) if word(24 + i * 8) == 0x3ff]
    if len(formats) != 1 or not 24 <= formats[0] <= header_size - 8:
        raise ValueError('Invalid XEX format header')
    start = formats[0]
    size, encryption, compression = struct.unpack_from('>IHH', data, start)
    if not 8 <= size <= header_size - start or encryption > 1 or compression > 1:
        raise ValueError('Unsupported XEX format for this port')
    payload = data[header_size:]
    if encryption:
        session = decrypt(data[security + 0x150:security + 0x160], RETAIL_KEY)
        payload = decrypt(payload, session)
    if compression == 0:
        if len(payload) < image_size:
            raise ValueError('Truncated XEX image')
        image = payload[:image_size]
    else:
        if size < 16 or (size - 8) % 8:
            raise ValueError('Invalid XEX block table')
        image = bytearray(image_size)
        source = destination = 0
        for offset in range(start + 8, start + size, 8):
            block_size, zeros = word(offset), word(offset + 4)
            if block_size > len(payload) - source or block_size + zeros > image_size - destination:
                raise ValueError('Invalid XEX block range')
            image[destination:destination + block_size] = payload[source:source + block_size]
            source += block_size
            destination += block_size + zeros
        # Security image size also includes the final zero-filled region.
        if len(payload) - source >= 16:
            raise ValueError('Incomplete XEX image or unexpected trailing payload')
        image = bytes(image)
    if image[:2] != b'MZ':
        raise ValueError('Decoded XEX image is invalid')
    return data[:header_size], image


def read_image(path: Path) -> bytes:
    return decode_xex(path.read_bytes())[1]
