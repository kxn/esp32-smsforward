"""Generate the firmware's flash-resident caller-origin tables.

The tables answer "where is this number from" for the three kinds of number the
modem can hand to the firmware: mainland mobile numbers (handled by the separate
phone_region_data.inc table), mainland landline numbers (area codes) and
international numbers (country calling codes). A fourth, small table names the
nationally published service numbers such as bank hotlines.

All data is pinned and hashed so a rebuild cannot silently pick up a different
upstream revision:

* landline area codes - @province-city-china/district-code 8.5.8, MIT licence
  https://github.com/uiwjs/province-city-china
* country calling codes - Google libphonenumber PhoneNumberMetadata.xml, pinned
  to a commit, Apache-2.0 licence
  https://github.com/google/libphonenumber
* Chinese country names - @province-city-china/country 8.5.8, MIT licence, plus
  a handful of missing entries filled in by hand below
* service numbers - tools/data/service_numbers.tsv, curated in-tree, and the
  file lists the public sources for every prefix
"""

import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import sys
from urllib.request import urlopen


AREA_REVISION = "8.5.8"
AREA_URL = (
    "https://cdn.jsdelivr.net/npm/@province-city-china/district-code@"
    f"{AREA_REVISION}/district-code.json"
)
AREA_HASH = "03690b26b2a8eb800dba9ac6979fcb8badc6f62c3d768e1b67263528e701d2f0"

COUNTRY_NAME_REVISION = "8.5.8"
COUNTRY_NAME_URL = (
    "https://cdn.jsdelivr.net/npm/@province-city-china/country@"
    f"{COUNTRY_NAME_REVISION}/country.json"
)
COUNTRY_NAME_HASH = "388ad7ea261b566e9277ad5df2ac520de2f7cd3d1b3cf6b9554712e53c801585"

METADATA_REVISION = "a82c3ec0ceafc4ff71feae633a180f23e3d8d336"
METADATA_URL = (
    "https://raw.githubusercontent.com/google/libphonenumber/"
    f"{METADATA_REVISION}/resources/PhoneNumberMetadata.xml"
)
METADATA_HASH = "92ad96b290d62b0d391d49fc41f7c7b98d2bcb256f1ad4b555c5c32aef7c9e2d"

ROOT = Path(__file__).resolve().parents[1]
SERVICES = ROOT / "tools/data/service_numbers.tsv"
OUTPUT = ROOT / "firmware/main/phone_origin_data.inc"

FLASH_LIMIT = 64 * 1024

# Country names the upstream list does not carry. The values are the usual
# Chinese names for these territories.
EXTRA_COUNTRY_NAMES = {
    "SS": "南苏丹",
    "AC": "阿森松岛",
    "XK": "科索沃",
    "BL": "圣巴泰勒米",
    "BQ": "荷属加勒比",
    "HK": "香港",
    "MO": "澳门",
    "TW": "台湾",
    "CW": "库拉索",
}

# Calling codes shared by several territories. libphonenumber marks the main
# territory of each such code; the ones below are worth spelling out.
COUNTRY_OVERRIDES = {
    1: "美国/加拿大",
    7: "俄罗斯/哈萨克斯坦",
}

# Calling codes that are not tied to a territory.
NON_GEO_CODES = {
    800: "国际免费电话",
    808: "国际业务",
    870: "国际海事卫星",
    878: "国际业务",
    881: "全球移动卫星",
    882: "国际网络",
    883: "国际网络",
    888: "国际业务",
    979: "国际付费业务",
}

# Districts whose area code is shared with a whole province or a well known
# cluster. Upstream lists every city separately, which reads poorly in a
# one-line notice.
AREA_LABEL_OVERRIDES = {
    "0898": "海南",
    "0731": "长沙/株洲/湘潭",
}

AREA_SUFFIXES = ("自治州", "地区", "林区", "盟", "市", "县", "区")
# Ethnic-group words that follow the real name in an autonomous prefecture.
# Stripping them turns 黔西南布依族苗族自治州 into 黔西南, which reads much
# better in a one-line notice.
ETHNIC_SUFFIXES = tuple(
    sorted(
        (
            "布依族", "苗族", "侗族", "彝族", "白族", "傣族", "景颇族", "哈尼族",
            "傈僳族", "拉祜族", "佤族", "纳西族", "羌族", "藏族", "回族", "蒙古族",
            "土家族", "朝鲜族", "壮族", "瑶族", "畲族", "黎族", "满族", "土族",
            "撒拉族", "东乡族", "保安族", "裕固族", "锡伯族", "达斡尔族",
            "鄂温克族", "鄂伦春族", "塔吉克族", "塔塔尔族", "乌孜别克族", "俄罗斯族",
            "水族", "仡佬族", "仫佬族", "毛南族", "京族", "布朗族", "阿昌族",
            "普米族", "怒族", "德昂族", "独龙族", "基诺族", "门巴族", "珞巴族",
            "高山族", "图瓦族", "柯尔克孜", "哈萨克", "维吾尔", "蒙古",
        ),
        key=len,
        reverse=True,
    )
)
MAX_LABEL_CHARS = 12


def fetch(url, expected_hash, description):
    print(f"fetching {description}: {url}", file=sys.stderr)
    with urlopen(url, timeout=180) as response:
        data = response.read()
    digest = sha256(data).hexdigest()
    if digest != expected_hash:
        raise ValueError(
            f"{description}: sha256 {digest} does not match the pinned {expected_hash}"
        )
    return data


def shorten_area_name(name):
    for suffix in AREA_SUFFIXES:
        if name.endswith(suffix) and len(name) > len(suffix):
            name = name[: -len(suffix)]
            break
    changed = True
    while changed and len(name) > 2:
        changed = False
        for ethnic in ETHNIC_SUFFIXES:
            if name.endswith(ethnic) and len(name) > len(ethnic) + 1:
                name = name[: -len(ethnic)]
                changed = True
                break
    return name


def shorten_country_name(name):
    # Drop a parenthetical alias: 福克兰群岛（马尔维纳斯） -> 福克兰群岛.
    head, _, _ = name.partition("（")
    return head.strip() or name


def build_area_codes(data):
    entries = json.loads(data)
    if not isinstance(entries, list) or len(entries) < 200:
        raise ValueError("Unexpected area code list")

    grouped = {}
    for entry in entries:
        code = str(entry["code"])
        name = str(entry["name"])
        if not re.fullmatch(r"0\d{2,3}", code):
            raise ValueError(f"Unexpected area code {code}")
        if not name or "\0" in name:
            raise ValueError(f"Invalid area name for {code}")
        grouped.setdefault(code, []).append(shorten_area_name(name))

    table = {}
    for code, names in grouped.items():
        if code in AREA_LABEL_OVERRIDES:
            label = AREA_LABEL_OVERRIDES[code]
        else:
            label = "/".join(names)
            if len(label) > MAX_LABEL_CHARS:
                label = names[0] + "等"
        table[int(code)] = label
    return table


def extract_countries(metadata):
    territories = []
    for tag in re.findall(r"<territory\b([^>]*)/?>", metadata.decode("utf-8")):
        ident = re.search(r'\bid="([^"]+)"', tag)
        code = re.search(r'\bcountryCode="(\d+)"', tag)
        if not ident or not code:
            continue
        territories.append((int(code.group(1)), ident.group(1), 'mainCountryForCode="true"' in tag))

    geo = [item for item in territories if item[1] != "001"]
    non_geo = sorted({code for code, ident, _ in territories if ident == "001"})
    main_for_code = {}
    for code, ident, is_main in geo:
        if is_main or code not in main_for_code:
            main_for_code[code] = ident
    if len(main_for_code) < 150:
        raise ValueError("Unexpected country code count")
    if set(non_geo) != set(NON_GEO_CODES):
        raise ValueError(f"Unexpected non-geographic codes {non_geo}")
    return main_for_code


def build_countries(metadata, names):
    main_for_code = extract_countries(metadata)
    missing = sorted(alpha2 for alpha2 in main_for_code.values() if alpha2 not in names)
    if missing:
        raise ValueError(f"Missing Chinese names for {missing}")
    table = {
        code: COUNTRY_OVERRIDES.get(code) or shorten_country_name(names[alpha2])
        for code, alpha2 in main_for_code.items()
    }
    table.update(NON_GEO_CODES)
    return table


def build_services():
    table = {}
    for line in SERVICES.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        number, label = line.split("\t")
        if not re.fullmatch(r"\d{3,5}", number):
            raise ValueError(f"Unexpected service number {number}")
        if not label or "\0" in label:
            raise ValueError(f"Invalid service label for {number}")
        if int(number) in table:
            raise ValueError(f"Duplicate service number {number}")
        table[int(number)] = label
    if len(table) < 40:
        raise ValueError("Unexpected service number count")
    return table


class Labels:
    def __init__(self):
        self.list = []
        self.ids = {}
        self.offsets = []
        self.size = 0

    def id_of(self, label):
        existing = self.ids.get(label)
        if existing is not None:
            return existing
        ident = len(self.list) + 1
        self.offsets.append(self.size)
        self.list.append(label)
        self.size += len(label.encode("utf-8")) + 1
        self.ids[label] = ident
        return ident


def c_label_pool(labels, name):
    pieces = []
    for label in labels.list:
        escaped = label.replace("\\", "\\\\").replace('"', '\\"')
        pieces.append(f'    "{escaped}\\0"')
    return f"static const char {name}[] =\n" + "\n".join(pieces) + ";\n"


def c_array(type_name, name, values, width=12):
    lines = [f"static const {type_name} {name}[] = {{"]
    for start in range(0, len(values), width):
        lines.append("    " + ", ".join(map(str, values[start : start + width])) + ",")
    return "\n".join(lines + ["};", ""])


def build_output(area_codes, countries, services):
    labels = Labels()
    bulk = labels.id_of("企业客服")

    area_numbers = sorted(area_codes)
    area_labels = [labels.id_of(area_codes[number]) for number in area_numbers]
    country_numbers = sorted(countries)
    country_labels = [labels.id_of(countries[number]) for number in country_numbers]
    service_numbers = sorted(services)
    service_labels = [labels.id_of(services[number]) for number in service_numbers]

    offsets = list(labels.offsets) + [labels.size]
    header = [
        "/* Generated by tools/gen_phone_origin.py. Do not edit.",
        " *",
        " * Landline area codes: Copyright (c) uiwjs, MIT licence",
        " *   https://github.com/uiwjs/province-city-china",
        f" *   @province-city-china/district-code {AREA_REVISION}",
        " * Country calling codes: Copyright (c) The Libphonenumber Authors,",
        " * Apache-2.0 licence",
        " *   https://github.com/google/libphonenumber",
        f" *   PhoneNumberMetadata.xml at {METADATA_REVISION}",
        " * Chinese country names: Copyright (c) uiwjs, MIT licence",
        f" *   @province-city-china/country {COUNTRY_NAME_REVISION}",
        " * Service numbers: curated in tools/data/service_numbers.tsv,",
        " * see that file for the public sources of each prefix.",
        " */",
        f"enum {{ ORIGIN_LABEL_COUNT = {len(labels.list)},",
        f"       ORIGIN_AREA_COUNT = {len(area_numbers)},",
        f"       ORIGIN_COUNTRY_COUNT = {len(country_numbers)},",
        f"       ORIGIN_SERVICE_COUNT = {len(service_numbers)},",
        f"       ORIGIN_BULK_LABEL = {bulk} }};",
        "",
    ]
    body = [
        c_label_pool(labels, "origin_labels"),
        c_array("uint16_t", "origin_label_offsets", offsets),
        c_array("uint16_t", "origin_area_codes", area_numbers),
        c_array("uint16_t", "origin_area_labels", area_labels),
        c_array("uint16_t", "origin_country_codes", country_numbers),
        c_array("uint16_t", "origin_country_labels", country_labels),
        c_array("uint32_t", "origin_service_numbers", service_numbers),
        c_array("uint16_t", "origin_service_labels", service_labels),
    ]
    text = "\n".join(header) + "\n".join(body)

    size = len(text.encode("utf-8"))
    print(
        f"labels {len(labels.list)}, pool {labels.size} B, area {len(area_numbers)}, "
        f"country {len(country_numbers)}, service {len(service_numbers)}, file {size} B",
        file=sys.stderr,
    )
    if size > FLASH_LIMIT:
        raise ValueError(f"Generated table is {size} B, above the {FLASH_LIMIT} B budget")
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify the checked-in file")
    parser.add_argument("--area-source", type=Path, help="use a local district-code.json")
    parser.add_argument("--metadata-source", type=Path, help="use a local metadata XML")
    parser.add_argument("--country-source", type=Path, help="use a local country.json")
    args = parser.parse_args()

    if args.area_source:
        area_data = args.area_source.read_bytes()
    else:
        area_data = fetch(AREA_URL, AREA_HASH, "landline area codes")
    if args.metadata_source:
        metadata = args.metadata_source.read_bytes()
    else:
        metadata = fetch(METADATA_URL, METADATA_HASH, "calling code metadata")
    if args.country_source:
        name_entries = json.loads(args.country_source.read_bytes())
    else:
        name_entries = json.loads(fetch(COUNTRY_NAME_URL, COUNTRY_NAME_HASH, "country names"))
    names = {entry["alpha2"]: entry["cnname"] for entry in name_entries}
    names.update(EXTRA_COUNTRY_NAMES)

    text = build_output(
        build_area_codes(area_data), build_countries(metadata, names), build_services()
    )

    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != text:
            raise SystemExit(f"{OUTPUT} is out of date; run python tools/gen_phone_origin.py")
        print(f"{OUTPUT.name} matches the generator", file=sys.stderr)
        return

    OUTPUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUTPUT}", file=sys.stderr)


if __name__ == "__main__":
    main()
