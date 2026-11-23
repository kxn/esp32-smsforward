# 手机号归属地数据

固件使用 [jieeit/phone v1.0.3](https://github.com/jieeit/phone/tree/v1.0.3) 的 `src/data/phone.dat`，版本固定为提交 `62ad757490c8cd71d7456101bf7ea2629ca62f13`。上游标注的数据日期为 2026 年 10 月 22 日。原文件 SHA-256 为 `115937ca789f98b854c0a61b63bfdf8950923f764575a236ac75e5d03081bcaf`。归属地数据版权为 Copyright (c) 2026 Jieeit，采用 MIT 许可证。[许可证全文](LICENSE-jieeit-phone.txt)

固件表保留原库的 522,410 个中国大陆手机七位号段和 371 个归属地标签。标签按上游的省份和城市拼接，两个字段文字相同时只显示一次。表中省市名称采用上游写法，未收录的号段没有归属地。归属地表示号段分配地，手机当前的位置可能不同。

固件在 Flash 中直接查找静态表。表按手机号前五位分组，并将组内连续、归属地相同的号段压缩成一条边界记录。每个边界用两字节保存组内位置和归属地编号。查找时二分定位边界并返回 UTF-8 字符串，无需解压。当前表占 330,247 字节，约 322.5 KiB。

常规构建直接使用 `firmware/main/phone_region_data.inc`。重新生成时可以下载固定版本，或提供已有的原文件。生成器会验证原文件哈希。

```sh
python tools/gen_phone_region.py
python tools/gen_phone_region.py --source /path/to/phone.dat --check
```
