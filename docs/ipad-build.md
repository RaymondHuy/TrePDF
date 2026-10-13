# Build và thử TrePDF trên iPad

TrePDF có target iPad riêng, yêu cầu iPadOS 17 trở lên. Build bằng Qt 6.11.0
for iOS và Xcode trên Mac. Workflow desktop `uat` và các thư viện macOS giữ
nguyên; workflow mới là `uat-ipad`, chạy thủ công với version `0.0.x` theo số
lần chạy của riêng workflow này. Re-run giữ version cũ.

## Tổ chức component theo platform

Các trang `Main`, `DocumentView` và page delegate chỉ có một bản dùng chung.
Component platform nằm trong `src/Components/ios/` theo cấu trúc:

```text
src/Components/ios/
  PlatformFileDialog.qml
  DocumentAccess.qml
  DocumentInput.qml
  HorizontalControlViewport.qml
  DialogMetrics.qml
  WindowMetrics.qml
  +ios/
    # Cùng sáu tên component ở trên, override cho iPad.
```

Bản ngoài `+ios` là hành vi mặc định cho desktop. `QQmlApplicationEngine`
dùng Qt File Selector để chọn bản `+ios` khi chạy trên iOS; shared QML luôn
gọi cùng tên component. CMake đóng gói cả hai bản, chỉ đăng ký type mặc định
để không trùng tên. Không copy nguyên trang hay chọn trang khác trong CMake.
Xem [Qt File Selector](https://doc.qt.io/qt-6/qfileselector.html) và
[QQmlApplicationEngine](https://doc.qt.io/qt-6/qqmlapplicationengine.html).

`src/platform/PlatformService.h` là API native chung; CMake chọn implementation
desktop trong `src/platform/` hoặc iOS trong `src/ios/`. Security scope, file
coordination và UIKit picker nằm trong `src/ios/IOSDocumentAccess.mm`.
`PDFFileOpenHandler` giữ nguyên, chỉ nhận đường dẫn local sau khi import.
Shared QML chỉ giữ các binding kích thước nhỏ dùng `Qt.platform.os`; logic
chọn file, chờ Save/export, bàn phím và cảm ứng nằm trong component platform.

Backend dùng cờ build `TREPDF_PDFIUM_NO_JAVASCRIPT` cho PDFium không V8 của
iPad. Cờ này bỏ tự kích hoạt JavaScript và trạng thái chờ tương ứng; build
desktop không bật cờ nên giữ hành vi form hiện có.

## Cài lên iPad của bạn bằng Xcode

Không cần membership trả phí để thử bằng Personal Team. Cần đăng nhập Apple
Account trong Xcode, kết nối iPad, Trust máy Mac và bật Developer Mode trên
iPad khi Xcode yêu cầu. Personal Team có profile hết hạn sau 7 ngày; lúc đó
build/cài lại. Không cần Developer ID hoặc notarization của macOS.

Máy hiện tại đã có Qt iOS ở `_deps/qt-ios/6.11.0/ios`, thư viện PDF ở
`_deps/ios/device`, và project Xcode ở `build/ipad/TrePDF.xcodeproj`.

1. Mở `build/ipad/TrePDF.xcodeproj` bằng Xcode.
2. Chọn scheme **trepdf**, target **trepdf** → **Signing & Capabilities**.
3. Bật **Automatically manage signing**, chọn Team của bạn. Đổi Bundle
   Identifier thành một tên riêng, ví dụ `com.tenban.trepdf`, nếu Apple báo trùng.
4. Chọn iPad đang kết nối làm thiết bị đích, rồi nhấn **Run**.

Để giữ Team/Bundle ID qua lần chạy CMake sau, cấu hình bằng script dưới đây
với `--bundle-id com.tenban.trepdf --team-id TEAMID10KY`. Không ghi Team hay
certificate riêng vào repository.

## Chuẩn bị trên một máy Mac mới

Cần Xcode đầy đủ (đã mở lần đầu để cài components), Python 3.12+, CMake và
Ninja trên PATH. Các lệnh chạy ở root repository:

```sh
python3 -m venv _deps/ios-tools
_deps/ios-tools/bin/python -m pip install aqtinstall==3.3.0 cmake==3.31.6 ninja==1.11.1.3
export PATH="$PWD/_deps/ios-tools/bin:$PATH"

python -m aqt install-qt mac desktop 6.11.0 clang_64 -O _deps/qt-ios -m qtshadertools qttasktree
python -m aqt install-qt mac ios 6.11.0 ios -O _deps/qt-ios -m qtshadertools qtimageformats qttasktree
python scripts/ci/prepare_ios_dependencies.py \
  --platform ios-device-arm64 --directory _deps/ios/device

python scripts/ci/build_ios.py \
  --qt-ios _deps/qt-ios/6.11.0/ios \
  --qt-host _deps/qt-ios/6.11.0/macos \
  --dependencies _deps/ios/device --configure-only \
  --bundle-id com.tenban.trepdf
open build/ipad/TrePDF.xcodeproj
```

Máy hiện tại dùng host Qt sẵn có, thay `--qt-host` bằng
`/Users/luongvanhuy/Qt/6.11.0/macos`. Device và simulator là các platform khác
nhau; không dùng thư viện macOS/simulator để link bản device dù đều ARM64.
Script dependency cũng hỗ trợ `ios-simulator-arm64` cho người có Qt simulator
ARM64 tương thích; workflow hiện build device.

## Chạy GitHub Actions

Vào **Actions → uat-ipad → Run workflow**. Chọn bundle ID và một chế độ:

| Chế độ | Artifact | Cách sử dụng |
| --- | --- | --- |
| `unsigned` | `.xcarchive.zip` | Kiểm tra build không cần secrets; không cài trực tiếp được trên iPad. Dùng source/Xcode ở trên để cài thử miễn phí. |
| `development` | `.ipa` | Apple Development certificate và development profile có UDID iPad; cài bằng Xcode Devices and Simulators. |
| `ad-hoc` | `.ipa` | Apple Distribution certificate và Ad Hoc profile có UDID iPad; cài trên những thiết bị đã đăng ký. |
| `app-store-connect` | `.ipa` | Apple Distribution certificate và App Store Connect profile; upload bằng Transporter để dùng TestFlight/App Store. |

Workflow chỉ build/xuất artifact, không tự upload lên App Store Connect.
Build thành công không có nghĩa profile cho phép mọi iPad cài đặt. Các chế độ
phân phối trả phí cần membership Apple Developer phù hợp; Personal Team dùng
đường Xcode local ở trên.

Để bật signed IPA, thêm 3 **GitHub Actions repository secrets**:

- `IOS_CERTIFICATE_P12_BASE64`: certificate kèm private key xuất thành `.p12`.
- `IOS_CERTIFICATE_PASSWORD`: mật khẩu không rỗng của `.p12`.
- `IOS_PROVISION_PROFILE_BASE64`: profile `.mobileprovision` còn hạn, có
  explicit App ID khớp bundle ID và đúng chế độ đã chọn.

Tạo chuỗi base64 một dòng trên Mac, sao chép trực tiếp vào GitHub Secrets:

```sh
base64 -i /path/to/certificate.p12 | tr -d '\n' | pbcopy
base64 -i /path/to/profile.mobileprovision | tr -d '\n' | pbcopy
```

Chạy từng lệnh và dán vào đúng secret trước khi chạy lệnh kế tiếp. Không đưa
certificate/private key, profile hay chuỗi base64 vào Git. Script dùng keychain
tạm, kiểm tra certificate khớp profile, rồi xóa các signing assets do nó tạo.

## Hành vi Files và cảm ứng

- **Open** nhập bản sao PDF vào `Documents/TrePDF/Imported` của ứng dụng.
  Đọc file từ provider/iCloud qua security scope và file coordination; PDFium
  luôn đọc file local, không nạp toàn bộ PDF vào RAM.
- **Save** cập nhật bản local trong TrePDF. **Save As** lưu thành bản local
  mới, rồi mở Files để xuất một bản sao. Hủy Files vẫn giữ bản local đã lưu;
  tài liệu gốc ở provider không bị ghi đè ngầm.
- Các bản local nằm trong **Files → On My iPad → TrePDF** để bạn mở lại hoặc
  quản lý. Khi mở lại qua Files, TrePDF nhập thành một bản làm việc mới.
- Xuất Excel cũng hoàn tất file local trước khi mở Files. Thêm ảnh nhập bản
  sao tạm để giải mã, không giữ quyền đọc provider vô thời hạn.
- Browse mặc định vuốt để cuộn, chụm hai ngón để zoom. Bật **Chọn văn bản**
  khi muốn kéo chọn và Copy; tắt lại để cuộn bằng ngón tay. Single/Facing đổi
  trang bằng thanh điều khiển dưới. Thanh công cụ/tab có thể cuộn ngang.
- Hộp thoại co theo vùng hiển thị/bàn phím; giao diện desktop giữ giá trị cũ.

PDFium iOS dùng bản **157.0.8100.0 không V8**, ghim SHA-256 riêng cho thiết bị
và simulator, để không yêu cầu quyền chạy JIT. JavaScript
nhúng trong PDF (ví dụ tự tính toán bằng script) chưa được hỗ trợ trên iPad.
Các form AcroForm thông thường vẫn dùng luồng hiện tại; XFA vốn không được hỗ trợ.
Chưa có kiểm thử thiết bị thật hay chứng chỉ/profile của bạn trong workspace.

## Checklist test trên máy thật

1. Open PDF local và iCloud; mở cùng lúc nhiều PDF, PDF có mật khẩu và file lớn.
2. Cuộn nhanh, zoom, xoay iPad, thu nhỏ cửa sổ; kiểm tra bàn phím không che input.
3. Sửa văn bản/ảnh, thêm ghi chú, ký tên, điền form rồi Save, đóng và mở lại.
4. Save As ra Files: thử cả chọn đích, hủy và provider lỗi; kiểm tra file gốc.
5. Xuất Excel, đặt mật khẩu, watermark; mở lại file xuất để xác minh.
6. Chuyển tab khi đang lưu, đưa app ra nền/quay lại, đóng và mở lại ứng dụng.

Nguồn: [Qt for iOS](https://doc.qt.io/qt-6.11/ios.html),
[Apple Personal Team](https://developer.apple.com/help/account/basics/about-your-developer-account),
[Apple certificate types](https://developer.apple.com/help/account/certificates/certificates-overview).
