# Watermark trong Protect

Protect → Watermark cung cấp Add, Update và Remove. Mỗi nhóm watermark có một
ID riêng và có thể áp dụng cho toàn tài liệu hoặc các trang như `1-3,5`.
Update/Remove chọn một nhóm; chỉ watermark hợp lệ do TrePDF tạo được quản lý.
Watermark từ ứng dụng khác và marker không hợp lệ giữ nguyên.

## Giao diện và trạng thái

`WatermarkDialog.qml` nhận controller của session được chụp khi mở dialog.
Đổi tab, session, tài liệu, cấu trúc trang hoặc bắt đầu Save sẽ đóng dialog.
Dialog hỗ trợ chữ Unicode, font, cỡ chữ, màu, ảnh cục bộ, opacity, góc xoay,
chín vị trí, lớp trước/sau nội dung và phạm vi trang. Preview là trang mẫu
theo tỷ lệ trang hiện tại; sau Apply, viewer hiển thị watermark trên PDF thật.

`PDFEditControllerWatermarks.cpp` giữ các nhóm đã quét và thay đổi theo source
page identity. Mỗi Apply/Remove là một command riêng trong lịch sử chung;
Undo/Redo/Discard và close guard dùng cùng cơ chế với các draft khác.
Dialog chỉ giữ thiết lập tạm; Cancel không tạo draft. Ảnh được chụp pixel ngay
khi chọn, nên thay đổi/xóa file gốc không ảnh hưởng đến preview hoặc Save.

Lưu hoặc hủy các draft nội dung/trang trước khi quản lý watermark. Khi còn
watermark chưa lưu, Edit bị khóa; Save/Discard khôi phục khả năng sửa nội dung.
Điều này giữ đúng thứ tự của watermark phía trước trong renderer PDF và các
draft overlay QML. Trang mới chèn cần Save trước khi nhận watermark.
Thay đổi mật khẩu có thể lưu cùng watermark. Sau Save, các thao tác nội dung
và trang thông thường vẫn sử dụng được; watermark không trở thành text/image
candidate của Edit.

## Artwork, nhận diện và hiển thị

`PDFWatermark.h/.cpp` cung cấp snapshot thuần dữ liệu dùng chung bởi controller,
scanner, render worker và writer. Chữ được chuyển thành một vector path trên
GUI thread, giữ hiển thị Unicode và không cần nhúng font khi Save. Chữ watermark
không phải lớp text có thể tìm kiếm/chọn. Ảnh dùng pixel snapshot có alpha;
opacity chỉ áp dụng một lần. Artwork được xoay, co vừa phần trang khả dụng và
căn theo vị trí đã chọn, với lề tối đa 12 PDF point.

Mỗi trang chứa một path/image object cho mỗi nhóm, có marked content
`TrePDFWatermark`. Metadata versioned lưu thiết lập và dữ liệu cần để cập nhật.
Scanner chỉ nhận object ở cấp trang, có cấu trúc, geometry và appearance phù
hợp metadata. Writer kiểm lại fingerprint trước khi cập nhật/xóa. Parser và
scanner giới hạn số trang, object, path segment, metadata và bộ nhớ ảnh.

`PDFPageItem` chụp các thay đổi theo trang cùng draft revision. Render worker áp
watermark vào preview document riêng sau suppression/move của source objects;
preview document luôn bị hủy sau batch. Form document và form values giữ nguyên.
Trước/sau nội dung dùng drawing order thật của PDF; annotation/form vẫn được
render qua pipeline hiện có. Cả preview và Save dùng phép ánh xạ PDFium để xử
lý CropBox và intrinsic page rotation. View rotation không ghi vào PDF.

## Persistence và giới hạn

`PDFSaveWriter::prepare()` chụp các batch theo trang. Writer dùng document riêng,
kiểm quyền sửa nội dung, xác thực source, tạo watermark và GenerateContent trên
trang bị đổi, rồi tiếp tục Save/Save As nguyên tử hiện có. Password-only fast
path loại trừ request có watermark. Save thành công và reopen mới xóa draft;
lỗi giữ nguyên draft và file đích. Save As giữ nguyên nguồn.

- Tối đa 32 nhóm và 10.000 trang trong một tài liệu được quét.
- Chữ tối đa 200 UTF-16 code unit; artwork path có giới hạn độ phức tạp.
- Ảnh nguồn tối đa 64 MiB; ảnh giải mã tối đa 4.194.304 pixel/16 MiB.
- Tổng ảnh của các nhóm đang quản lý tối đa 64 MiB.
- Không phát hiện/xóa watermark tùy ý từ phần mềm khác, watermark trong Form
  XObject hoặc marker đã bị sửa không còn khớp artwork.
- Watermark là nội dung nhìn thấy, không đặt quyền hạn chế sao chép/chỉnh sửa.

## Kiểm thử

`tests/tst_PDFEditControllerWatermarks.cpp` kiểm history/session, text và ảnh
trong suốt, Save/reopen/update/remove, lỗi atomic, mật khẩu, guard và đối chiếu
preview với output ở bốn góc xoay, cả hai lớp, với CropBox lệch. Các test cũng
kiểm tra khởi tạo dialog sau async reopen, nhận diện sau sửa text/chèn trang,
fingerprint đã thay đổi và bảo toàn watermark lạ/metadata lỗi. Các test thuộc
`trepdf_edit_controller_tests`; UI cần build app, cập nhật bản dịch và qmllint.
Thay đổi renderer còn cần smoke test native Metal cho zoom/scroll, đổi tab,
close/reopen và Quit.
