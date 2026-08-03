#!env python3

from __future__ import annotations

from collections.abc import Iterable, MutableSequence, Iterator
from dataclasses import dataclass, field, fields
import sys
from typing import overload, Self, cast, final, override
from base64 import b64encode, b64decode
from zlib import crc32


@final
class BitField:
    def __init__(self, bits: int):
        self.bits = bits
        self.private_name = ""

    def __set_name__(self, owner: object, name: str):
        self.private_name = "_" + name

    @overload
    def __get__(self, obj: None, objtype: type | None = None) -> Self: ...

    @overload
    def __get__(self, obj: object, objtype: type | None = None) -> int: ...

    def __get__(self, obj: object | None, objtype: type | None = None):
        if obj is None:
            return self
        if not hasattr(obj, self.private_name):
            setattr(obj, self.private_name, 0)
        return cast(int, getattr(obj, self.private_name))

    def __set__(self, obj: object, value: int) -> None:
        if not (0 <= value < (1 << self.bits)):
            raise ValueError(f"{value} exceeds {self.bits} bits")
        setattr(obj, self.private_name, value)

    def __len__(self):
        return self.bits


@final
class BitFieldArrayView(MutableSequence[int]):
    def __init__(
        self, obj: object, private_name: str, bits: int, size: int, default: int
    ):
        self._obj = obj
        self._private_name = private_name
        self._bits = bits
        self._size = size
        self._default = default
        if not hasattr(obj, private_name):
            setattr(obj, private_name, [default] * size)
        else:
            data = cast(object, getattr(obj, private_name))
            if not isinstance(data, list) or len(cast(list[int], data)) != size:
                raise TypeError("backing storage corrupted")

    def _data(self) -> list[int]:
        return cast(list[int], getattr(self._obj, self._private_name))

    def _check_index(self, i: int) -> None:
        if not (0 <= i < self._size):
            raise IndexError(f"index {i} out of range (0..{self._size - 1})")

    def _check_value(self, v: int | object) -> None:
        if not isinstance(v, int):
            raise TypeError("value must be int")
        if not (0 <= v < (1 << self._bits)):
            raise ValueError(f"{v} exceeds {self._bits} bits")

    @override
    def __len__(self) -> int:
        return self._size

    @overload
    def __getitem__(self, i: int) -> int: ...
    @overload
    def __getitem__(self, i: slice) -> list[int]: ...

    @override
    def __getitem__(self, i: int | slice):
        data = self._data()
        if isinstance(i, slice):
            return data[i]
        self._check_index(i)
        return data[i]

    @override
    def __setitem__(self, i: int | slice, v: int | Iterable[int]) -> None:
        data = self._data()

        if isinstance(i, slice):
            vals = list(cast(Iterable[int], v))
            if len(vals) != len(range(*i.indices(self._size))):
                raise ValueError("slice assignment length mismatch")
            for x in vals:
                self._check_value(x)
            data[i] = vals
            return

        self._check_index(i)
        iv = cast(int, v)
        self._check_value(iv)
        data[i] = iv

    @override
    def __delitem__(self, i: int | slice) -> None:
        raise TypeError("fixed-size bitfield array")

    @override
    def insert(self, index: int, value: int) -> None:
        raise TypeError("fixed-size bitfield array")

    @override
    def __iter__(self) -> Iterator[int]:
        return iter(self._data())

    @override
    def __repr__(self) -> str:
        return repr(self._data())


@final
class BitFieldArray:
    def __init__(self, bits: int, size: int, *, default: int = 0):
        self.bits = bits
        self.size = size
        self.default = default
        self.private_name = ""

        if not (0 <= default < (1 << bits)):
            raise ValueError("default exceeds bits")

    def __set_name__(self, owner: type, name: str) -> None:
        self.private_name = "_" + name

    @overload
    def __get__(self, obj: None, objtype: type | None = None) -> Self: ...

    @overload
    def __get__(
        self, obj: object, objtype: type | None = None
    ) -> BitFieldArrayView: ...

    def __get__(self, obj: object | None, objtype: type | None = None):
        if obj is None:
            return self
        return BitFieldArrayView(
            obj, self.private_name, self.bits, self.size, self.default
        )

    def __set__(self, obj: object, value: list[int]) -> None:
        if not isinstance(value, list) or len(value) != self.size:  # pyright: ignore[reportUnnecessaryIsInstance]
            raise ValueError(f"length must be {self.size}")
        for v in value:
            if not isinstance(v, int) or not (0 <= v < (1 << self.bits)):  # pyright: ignore[reportUnnecessaryIsInstance].
                raise ValueError(f"{v} exceeds {self.bits} bits")
        setattr(obj, self.private_name, value)

    def __len__(self) -> int:
        return self.bits * self.size


@dataclass
class Vareg:
    disc3_bi: BitField = field(default=BitField(3), init=False)
    Ioffset: BitField = field(default=BitField(3), init=False)
    obi: BitField = field(default=BitField(3), init=False)
    ibuf: BitField = field(default=BitField(3), init=False)
    pre_bias: BitField = field(default=BitField(3), init=False)
    sbi: BitField = field(default=BitField(3), init=False)
    vrc: BitField = field(default=BitField(3), init=False)
    ifsf: BitField = field(default=BitField(3), init=False)
    ifss: BitField = field(default=BitField(3), init=False)
    sha_bias: BitField = field(default=BitField(3), init=False)
    twbi: BitField = field(default=BitField(3), init=False)
    ck_bi: BitField = field(default=BitField(4), init=False)
    Iramp: BitField = field(default=BitField(4), init=False)
    ifp: BitField = field(default=BitField(4), init=False)
    vth: BitField = field(default=BitField(5), init=False)
    Pos_II_2: BitField = field(default=BitField(1), init=False)
    Pos_II_1: BitField = field(default=BitField(1), init=False)
    Shabi_lg: BitField = field(default=BitField(1), init=False)
    Test_Enable: BitFieldArray = field(default=BitFieldArray(1, 64), init=False)
    Trim_Dac: BitFieldArray = field(default=BitFieldArray(4, 64), init=False)
    CH_Disable: BitFieldArray = field(default=BitFieldArray(1, 64), init=False)
    DTHR: BitField = field(default=BitField(10), init=False)
    Dis_chan_CM: BitFieldArray = field(default=BitFieldArray(1, 64), init=False)
    Dis_chan_CMDummy: BitField = field(default=BitField(1), init=False)
    Del_reg: BitFieldArray = field(default=BitFieldArray(6, 64), init=False)
    Del_reg_Dummy: BitField = field(default=BitField(6), init=False)
    ADC_test2: BitField = field(default=BitField(1), init=False)
    ADC_test1: BitField = field(default=BitField(1), init=False)
    Ileak_offset: BitField = field(default=BitField(1), init=False)
    Reserved6: BitField = field(default=BitField(1), init=False)
    VA_RO: BitField = field(default=BitField(1), init=False)
    ADC_on_b: BitField = field(default=BitField(1), init=False)
    Reserved5: BitField = field(default=BitField(1), init=False)
    negQ: BitField = field(default=BitField(1), init=False)
    Low_gain: BitField = field(default=BitField(1), init=False)
    Test_on: BitField = field(default=BitField(1), init=False)
    CC_on: BitField = field(default=BitField(1), init=False)
    Nside: BitField = field(default=BitField(1), init=False)
    Slew_on_b: BitField = field(default=BitField(1), init=False)
    Cal_gen_on: BitField = field(default=BitField(1), init=False)
    Preb_hp: BitField = field(default=BitField(1), init=False)
    Ck_en: BitField = field(default=BitField(1), init=False)
    RO_all: BitField = field(default=BitField(1), init=False)
    CM_thr_dis: BitField = field(default=BitField(1), init=False)
    Iramp_f2: BitField = field(default=BitField(1), init=False)
    Iramp_fb: BitField = field(default=BitField(1), init=False)
    Reserved4: BitField = field(default=BitField(1), init=False)
    Reserved3: BitField = field(default=BitField(1), init=False)
    Reserved2: BitField = field(default=BitField(1), init=False)
    Reserved1: BitField = field(default=BitField(1), init=False)
    All2: BitField = field(default=BitField(1), init=False)
    Cal_HDR: BitField = field(default=BitField(1), init=False)
    Cal_HDR2: BitField = field(default=BitField(1), init=False)
    C_cal6: BitField = field(default=BitField(1), init=False)
    C_cal5: BitField = field(default=BitField(1), init=False)
    C_cal4: BitField = field(default=BitField(1), init=False)
    C_cal3: BitField = field(default=BitField(1), init=False)
    C_cal2: BitField = field(default=BitField(1), init=False)
    C_cal1: BitField = field(default=BitField(1), init=False)
    C_cal0: BitField = field(default=BitField(1), init=False)

    def _flatten(self) -> list[tuple[int, int]]:
        out: list[tuple[int, int]] = []
        cls = type(self)
        for f in fields(self):
            name = f.name
            spec = cast(object, getattr(cls, name))

            if isinstance(spec, BitField):
                bits = len(spec)
                value = cast(int, getattr(self, name))
                out.append((bits, value))
                continue

            if isinstance(spec, BitFieldArray):
                bits_per = spec.bits
                size = spec.size
                arr = cast(list[int], getattr(self, name))
                for i in range(size - 1, -1, -1):
                    out.append((bits_per, arr[i]))
                continue
            raise TypeError(f"Unknown field type: {name}")
        return out

    def show(self, title: str) -> None:
        for f in fields(self):
            spec = cast(object, getattr(type(self), f.name))
            if isinstance(spec, BitField):
                print(f"{title}({f.name}): {cast(object, getattr(self, f.name))}")
            elif isinstance(spec, BitFieldArray):
                arr = cast(list[int], getattr(self, f.name))
                for i in reversed(range(len(arr))):
                    print(f"{title}({f.name}[{i}]): {arr[i]}")

    def to_bytearray(self) -> bytearray:
        flat = self._flatten()
        out = bytearray(128)
        bit_pos = 0

        for bits, value in flat:
            for i in range(bits):
                if value & (1 << (bits - 1 - i)):
                    p = bit_pos + i
                    byte_index = p // 8
                    bit_index = 7 - (p % 8)  # ★ MSB=0
                    out[byte_index] |= 1 << bit_index
            bit_pos += bits

        return out

    def from_bytearray(self, data: bytearray) -> None:
        bit_pos = 0

        for f in fields(self):
            name = f.name
            spec = cast(object, getattr(type(self), name))

            if isinstance(spec, BitField):
                bits = len(spec)
                value = 0
                for i in range(bits):
                    p = bit_pos + i
                    byte_index = p // 8
                    bit_index = 7 - (p % 8)  # ★ MSB=0
                    if data[byte_index] & (1 << bit_index):
                        value |= 1 << (bits - i - 1)
                setattr(self, name, value)
                bit_pos += bits
                continue

            if isinstance(spec, BitFieldArray):
                bits_per = spec.bits
                size = spec.size
                arr: list[int] = []

                for _ in range(size):
                    value = 0
                    for i in range(bits_per):
                        p = bit_pos + i
                        byte_index = p // 8
                        bit_index = 7 - (p % 8)  # ★ MSB=0
                        if data[byte_index] & (1 << bit_index):
                            value |= 1 << (bits_per - i - 1)
                    arr.append(value)
                    bit_pos += bits_per
                arr.reverse()
                setattr(self, name, arr)
                continue


@dataclass
class Vareg4ASIC:
    asic0: Vareg = field(default_factory=Vareg)
    asic1: Vareg = field(default_factory=Vareg)
    asic2: Vareg = field(default_factory=Vareg)
    asic3: Vareg = field(default_factory=Vareg)

    def to_bytearray(self) -> bytearray:
        out = bytearray()
        for v in (self.asic3, self.asic2, self.asic1, self.asic0):
            out.extend(v.to_bytearray())
        return out

    def from_bytearray(self, data: bytearray) -> None:
        if len(data) != 128 * 4:
            raise ValueError("data length must be 512 bytes")
        self.asic3.from_bytearray(data[0:128])
        self.asic2.from_bytearray(data[128:256])
        self.asic1.from_bytearray(data[256:384])
        self.asic0.from_bytearray(data[384:512])

    def show(self) -> None:
        self.asic0.show("asic0")
        self.asic1.show("asic1")
        self.asic2.show("asic2")
        self.asic3.show("asic3")

    def to_base64(self) -> str:
        data = self.to_bytearray()
        crc = crc32(data) & 0xFFFFFFFF
        data.extend(crc.to_bytes(4))
        return b64encode(data).decode()

    def from_base64(self, data: str, check: bool = True) -> None:
        raw = b64decode(data)
        if len(raw) != 512 + 4:
            raise ValueError("decoded data length must be 516 bytes")
        if check:
            data_crc = int.from_bytes(raw[512:516])
            calc_crc = crc32(raw[0:512]) & 0xFFFFFFFF
            if data_crc != calc_crc:
                raise ValueError("CRC32 mismatch")
        self.from_bytearray(bytearray(raw[0:512]))

    def load(self, filename: str, check: bool = True) -> None:
        with open(filename, "rb") as f:
            raw = f.read()
        self.from_base64(raw.decode(), check=check)

    def save(self, filename: str) -> None:
        with open(filename, "wb") as f:
            _ = f.write(self.to_base64().encode())


if __name__ == "__main__":
    args = sys.argv[1:]

    if ("-h" in args) or ("--help" in args) or (len(args) not in (1, 4)):
        print("Usage:")
        print("  vareg.py <input_file>")
        print("  vareg.py <input_file> asic_number reg_name value")
        sys.exit(1)

    if len(args) == 1:
        vareg = Vareg4ASIC()
        vareg.load(args[0])
        vareg.show()
    elif len(args) == 4:
        vareg = Vareg4ASIC()
        vareg.load(args[0])
        asic_number = int(args[1])
        reg_name = args[2]
        value = int(args[3])
        if not (0 <= asic_number <= 3):
            print("ASIC number must be 0-3")
            sys.exit(1)
        asic = [vareg.asic0, vareg.asic1, vareg.asic2, vareg.asic3][asic_number]
        if not hasattr(asic, reg_name):
            print(f"Register {reg_name} not found")
            sys.exit(1)
        setattr(asic, reg_name, value)
        vareg.save(args[0])
        print(f"Set ASIC{asic_number} {reg_name} to {value}")
