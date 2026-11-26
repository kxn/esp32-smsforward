# 来电与短信来源数据

固件除了手机号归属地（见 [PHONE_REGION_DATA.md](PHONE_REGION_DATA.md)）之外，还会识别另外三类号码，并把结果附在短信、来电和彩信通知的号码后面：

| 号码形态 | 例子 | 通知里的标注 |
| --- | --- | --- |
| 中国大陆手机号 | `13800138000` | 号段归属省市，如 `北京` |
| 中国大陆座机 | `01021285387` | 区号对应的城市，如 `北京`；`0755…` 为 `深圳` |
| 全国统一服务号 | `95555`、`10086`、`110` | 机构名称，如 `招商银行`、`中国移动`、`公安报警` |
| 400 / 800 企业号 | `4001234567` | `企业客服`（具体企业不对公众公布） |
| 境外号码 | `+14155551234`、`0014155551234` | 国家或地区，如 `美国/加拿大`、`香港` |

例如：

```
【接到来自01021285387（北京）的来电】
【接到来自95555（招商银行）的来电】
【收到来自+14155551234（美国/加拿大）的短信】
```

标注只表示号码的登记地或登记机构，不代表来电者当前的位置。未收录的号段、不可识别的号码和短号不加括号。

## 数据来源

| 数据 | 来源 | 版本 | 许可证 |
| --- | --- | --- | --- |
| 大陆座机区号 | [uiwjs/province-city-china](https://github.com/uiwjs/province-city-china) 的 `@province-city-china/district-code` | 8.5.8 | MIT |
| 国际电话区号 | Google [libphonenumber](https://github.com/google/libphonenumber) 的 `PhoneNumberMetadata.xml` | 提交 `a82c3ec0ceafc4ff71feae633a180f23e3d8d336` | Apache-2.0 |
| 国家中文名 | [uiwjs/province-city-china](https://github.com/uiwjs/province-city-china) 的 `@province-city-china/country`，另补 9 个上游缺项 | 8.5.8 | MIT |
| 服务号码 | 仓库内 `tools/data/service_numbers.tsv` | 随仓库 | CC0 |

服务号码只收录能核对到公开来源的号码，`tools/data/service_numbers.tsv` 的文件头列出了每个来源：中国人民银行的银行客服电话名单、国家金融监督管理总局的保险客服电话、工业和信息化部的运营商客服电话、国家邮政局的快递企业投诉电话，以及各省市公开的 `123xx` 便民热线清单。拿不准的号码一律不收录，宁可显示不出名字，也不要在通知里写错机构。

国际区号取自 libphonenumber 的权威表：一个区号对应多个国家或地区时（如 `+1`、`+7`、`+44`），表中用上游标记的"主归属"名称，`+1`、`+7` 另外写成 `美国/加拿大`、`俄罗斯/哈萨克斯坦`；`+800`、`+870`、`+881` 等不落到具体国家或地区的区号显示为国际业务名称。

## 表结构与查找

`firmware/main/phone_origin_data.inc` 是生成文件，包含四张排序表（座机区号 329 条、国际区号 215 条、服务号码 73 条，以及共享的 615 条 UTF-8 标签池）和 16 位标签编号。查找全部在 Flash 上做二分，不需要解压，也不占用堆。当前整份表约 23 KB 源码，编译后约 12 KB。

`firmware/main/phone_origin.c` 先把号码归一化：去掉空格、短横线、括号和点号；`+` 开头或 `00` 开头视为国际号码；`+86`、`0086` 和裸 `86` 前缀的大陆号码还原成 11 位后再查号段表。归一化之后按下面的顺序判断，先命中先返回：

1. 国际号码：按 3→2→1 位从长到短匹配国际区号。
2. 11 位、以 `1` 开头：大陆手机号，交给 `phone_region_lookup()`。
3. 3–5 位且命中服务号表：服务号码。
4. 10 位且以 `400`、`800` 开头：企业客服。
5. 以 `0` 开头且长度至少 7 位：大陆座机，先按 4 位区号、再按 3 位区号查表。

## 重新生成

```sh
python tools/gen_phone_origin.py
python tools/gen_phone_origin.py --check        # 校验仓库里的生成文件是否最新
python tools/gen_phone_origin.py --area-source district-code.json \
    --metadata-source PhoneNumberMetadata.xml --country-source country.json
```

生成器会验证每个下载文件的 SHA-256。修改服务号码只需编辑 `tools/data/service_numbers.tsv` 再运行生成器。生成结果超过 64 KiB 时生成器会报错，避免无声地吃掉 Flash。

`tools/test_phone_origin.c` 覆盖了上述每一类号码的典型写法（含带分隔符、`+86`/`0086`/裸 `86` 前缀、未收录的区号、`+999` 等无效国际区号），`python tools/test_host.py` 会连同通知文案测试一起运行。
