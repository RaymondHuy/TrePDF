# Catalogue TeX Gyre đã đối chiếu

Đối chiếu ngày 24-09-2026 với bytes tải trực tiếp qua HTTPS từ
`https://mirror.ctan.org/fonts/tex-gyre/opentype/<file>`.
Các file đều có signature `OTTO`, outline `CFF ` tĩnh và không có `glyf`.
Bản font không được chỉnh sửa hoặc chuyển đổi trước khi tính checksum.

## Nguồn và giấy phép

[CTAN TeX Gyre](https://ctan.org/pkg/tex-gyre) ghi giấy phép GUST Font License.
README riêng của từng family cũng xác nhận GFL và chỉ ra manifest của package:

| Family | Version upstream | README chính thức |
| --- | --- | --- |
| Cursor | 2.004 | [README Cursor](https://mirror.ctan.org/fonts/tex-gyre/doc/README-TeX-Gyre-Cursor.txt) |
| Heros | 2.004 | [README Heros](https://mirror.ctan.org/fonts/tex-gyre/doc/README-TeX-Gyre-Heros.txt) |
| Pagella | 2.501 | [README Pagella](https://mirror.ctan.org/fonts/tex-gyre/doc/README-TeX-Gyre-Pagella.txt) |
| Schola | 2.005 | [README Schola](https://mirror.ctan.org/fonts/tex-gyre/doc/README-TeX-Gyre-Schola.txt) |
| Termes | 2.004 | [README Termes](https://mirror.ctan.org/fonts/tex-gyre/doc/README-TeX-Gyre-Termes.txt) |

GFL cho phép dùng và phân phối theo LPPL 1.3c hoặc mới hơn. Thông báo đầy đủ:
[GFL](https://ctan.org/license/gfl),
[LPPL 1.3c](https://www.latex-project.org/lppl/lppl-1-3c/),
[package TeX Gyre nguyên bản](https://mirror.ctan.org/fonts/tex-gyre.zip).
Thông báo fixture nằm trong `tests/fixtures/fonts/tex-gyre/NOTICE.md`;
19 font còn lại không được đưa vào repository hay bundle ứng dụng.

## Ngoại lệ metadata có giới hạn

Cursor, Heros, Schola và Termes nguyên bản đặt đồng thời Preview & Print và
Editable (`fsType = 0x000c`) trong OS/2 version 3. Đây là metadata không tuân
quy tắc mutually exclusive của version 3 trở lên trong
[OpenType OS/2 specification](https://learn.microsoft.com/en-us/typography/opentype/spec/os2#fstype).
Cách đọc least-restrictive chỉ được specification cho phép với version 0–2;
không mở rộng cách đọc đó cho mọi font version 3.

`ReviewedCtanFonts::permitsEditableEmbedding()` là ngoại lệ dựa trên giấy phép
GFL đã đối chiếu, chỉ nhận đúng 16 file nguyên bản có size và SHA-256 bên dưới,
signature `OTTO`, OS/2 version 3 và `fsType = 0x000c`. File bị đổi dù một byte
không còn thỏa ngoại lệ. Không nhận theo family name, URL, metadata do file tự
khai, hay cờ cấp quyền do caller tùy ý truyền. Generic policy vẫn từ chối
Restricted, Preview & Print-only, no-subsetting, bitmap-only và reserved flags.
Không sửa `fsType`, bảng font hay checksum của bytes được tải/nhúng.

Pagella 2.501 có OS/2 version 4 và `fsType = 0`, nên đi qua quy tắc Installable
embedding thông thường. Khi upstream thay đổi các file cần ngoại lệ, bản mới
phải được đối chiếu lại trước khi thêm digest; không tự tin cậy bytes mới chỉ
vì dùng cùng URL.

## Manifest checksum

| File | Byte | OS/2 version | fsType | SHA-256 |
| --- | ---: | ---: | --- | --- |
| `texgyrecursor-bold.otf` | 87288 | 3 | `0x000c` | `95e9755bfa05759e8d1f8fba21a919ba46091e1275aeb5bf831dffd9c93b041e` |
| `texgyrecursor-bolditalic.otf` | 90360 | 3 | `0x000c` | `6aa5c41f489b898aa890c543e7dcd5e732c63894babc65569e01ee7e46533d82` |
| `texgyrecursor-italic.otf` | 87536 | 3 | `0x000c` | `545f24a2a9e5dd5ace865fe5e80134f55a37794b36e26abae5424aebff600056` |
| `texgyrecursor-regular.otf` | 84652 | 3 | `0x000c` | `0667deb48aa0e88be8f499c4d308e8b9116f290e7f969b0f5a34ee15c9644272` |
| `texgyreheros-bold.otf` | 135204 | 3 | `0x000c` | `b170162835f4efc288886dd4231406dc47e19b614cf4416836635599d44a7d60` |
| `texgyreheros-bolditalic.otf` | 135264 | 3 | `0x000c` | `166fc6d068d9c9974281555cb3d730365537a9b676ab269bb5163f5a75496505` |
| `texgyreheros-italic.otf` | 139208 | 3 | `0x000c` | `6473df7fa107b3fb4be38973710afe22b0640c2ac076d5337cf126bed9aa108c` |
| `texgyreheros-regular.otf` | 133600 | 3 | `0x000c` | `6ae1a09d5a940367b7aaaa91ee8bd8a2c333bfe193e7096e23f931357d62081f` |
| `texgyrepagella-bold.otf` | 219564 | 4 | `0x0000` | `dd4f9b69f0b24f1fb9be8634abf1fce2009e5c68a30842a3e9f5d6e535900abd` |
| `texgyrepagella-bolditalic.otf` | 216296 | 4 | `0x0000` | `556fd64655a92bd9f5485ae2c03f7b9cdaf745a27a91d7edb3c3abde997206d2` |
| `texgyrepagella-italic.otf` | 221936 | 4 | `0x0000` | `069bb27f48eb98a741715227fc6e4b7baf3639c29fc13925f70d1ed8884ac29a` |
| `texgyrepagella-regular.otf` | 218100 | 4 | `0x0000` | `44e64260716d8f2bbe412baa1ee99b7c995190ac4573177c24def0b9200438c7` |
| `texgyreschola-bold.otf` | 141752 | 3 | `0x000c` | `988c7b2a0ff0eae77d1df3338751b3c47a5e759117734a375b8e7f9de80e698b` |
| `texgyreschola-bolditalic.otf` | 144864 | 3 | `0x000c` | `95f085da44c04817771f2a3a754b6eeb442b9a79e79f51dc3b0612e3cc0d75cc` |
| `texgyreschola-italic.otf` | 143152 | 3 | `0x000c` | `2f45b8c394037951aaec73d947f0b0c3af715950f8f207c9f4290ba37a53a4d2` |
| `texgyreschola-regular.otf` | 140596 | 3 | `0x000c` | `935b82e25f56b1d1276ca82793205e8ce254fbb37ebd38bedf7388cef21fbf44` |
| `texgyretermes-bold.otf` | 139480 | 3 | `0x000c` | `2fb3e952065fa153c7e4e64e04b98b9d79225739b6025aa3f0f0782d299ff61e` |
| `texgyretermes-bolditalic.otf` | 144180 | 3 | `0x000c` | `1bf6af99cb0e26c12951317032d79b96ae009551e59ccf02a5b24f325ecfec87` |
| `texgyretermes-italic.otf` | 141324 | 3 | `0x000c` | `6dd103a1672e50568cd2f8a706ccd48443d44d7d073a59d2286f4e6f746575d6` |
| `texgyretermes-regular.otf` | 140932 | 3 | `0x000c` | `cc3fe7c707b81428d23d54df3eadd9228a2bf6a4d43125d94df56f5f63134659` |
