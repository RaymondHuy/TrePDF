# Hướng dẫn toàn bộ tệp C++

Tài liệu này mô tả 16 tệp triển khai C++ trong `src/`, tệp kiểm thử C++ trong
`tests/`, các header đi cùng và hai header hỗ trợ không có `.cpp` riêng. Nội dung
tập trung vào trách nhiệm thật của mỗi tệp, ownership, thread, dữ liệu trao đổi
và ranh giới không nên phá khi sửa code.

## Quy ước kiến trúc C++

- Controller và `PDFDocument` sống trên GUI thread; QML chỉ gọi API Qt/QML được
  expose, không truy cập handle PDFium trực tiếp.
- PDFium không thread-safe ngay cả khi dùng các document handle khác nhau. Mọi
  transaction PDFium phải đi qua `PDFiumProcessLock` toàn tiến trình.
- Render, form runtime và save mở document handle riêng; không dùng
  `FPDF_DOCUMENT` thuộc `PDFDocument` trên worker.
- Mọi kết quả bất đồng bộ phải được kiểm request id/generation/document key/
  revision trước khi áp vào object GUI.
- Save request là snapshot bất biến, không giữ con trỏ controller/document sang
  worker.

## Khởi động, font và item hỗ trợ

### [`src/main.cpp`](../src/main.cpp)

**Vai trò:** entry point của ứng dụng.

- Tạo `QGuiApplication`, đặt application name `TrePDF` và organization
  `TreOffice`.
- Gọi `ApplicationFonts::registerBundledFonts()` trước khi QML tạo font control,
  để danh mục font và face do app bundle cung cấp ổn định ngay từ đầu.
- Ép Qt Quick Controls dùng style `Basic`, phù hợp các control QML tự vẽ nền/
  content item.
- Tạo `QQmlApplicationEngine`, đặt UI language mặc định là `vi`, xử lý lỗi tạo
  root object và load `Main` từ module `TrePDF`.
- Chạy event loop. Tệp này không khởi tạo document/controller thủ công; các type
  được QML module/plugin đăng ký từ khai báo QML element trong class tương ứng.

### [`src/ApplicationFonts.cpp`](../src/ApplicationFonts.cpp) và
[`src/ApplicationFonts.h`](../src/ApplicationFonts.h)

**Vai trò:** nguồn sự thật cho font có thể hiển thị và có thể lưu an toàn vào PDF.

**Khởi tạo registry:**

- Chụp danh mục system family/face trước khi đăng ký bundled fonts để phân biệt
  nguồn system với resource app dù tên family trùng nhau.
- Đăng ký font dưới `src/assets/fonts`, ghi nhận exact face theo family, bold và
  italic; bundled face được ưu tiên khi giải quyết font draft.
- Cache danh sách family đã lọc để font picker không chạy probe CoreText nặng cho
  mỗi lần mở menu.

**Kiểm tra font dùng khi Save:**

- Trên macOS, dùng CoreText lấy đúng face bytes thay vì suy đoán từ family name.
- Nếu nguồn là TTC/collection, trích face cần dùng thành sfnt độc lập để PDFium
  không chọn nhầm face.
- Chỉ chấp nhận static TrueType `glyf/loca`, kích thước tối đa 64 MiB và quyền
  embedding Installable hoặc Editable theo OS/2.
- Loại CFF/CFF2, variable, color/bitmap-only, face thiếu glyph, Restricted và
  Preview & Print-only; trả error có loại để Save dừng rõ ràng thay vì substitute.
- Kiểm coverage theo Vietnamese/English/Chinese/Japanese/Korean cho filter UI,
  đồng thời vẫn kiểm chính xác glyph của nội dung ở bước chuẩn bị save.

**Hiệu chuẩn hiển thị:**

- Đo mapping từ target PDF horizontal scale sang `QFont::setStretch()` vì stretch
  của Qt/CoreText không tuyến tính cho mọi face.
- Cache calibration theo family/style/pixel size để `EditOverlay` và
  `PDFSaveWriter` dùng cùng phép đo, tránh preview khác layout khi ghi.

**API dữ liệu:** `FontLanguage`, `FontFaceData`, danh sách bundled/system family,
font bytes, glyph/face probe và helper stretch. Không giữ handle PDFium.

### [`src/DraftImageItem.cpp`](../src/DraftImageItem.cpp) và
[`src/DraftImageItem.h`](../src/DraftImageItem.h)

**Type QML:** `DraftImageView`, kế thừa `QQuickPaintedItem`.

- Expose property `sourceImage` dạng `QVariant`; chấp nhận snapshot `QImage` do
  controller đã decode và giới hạn kích thước.
- `paint()` vẽ ảnh với smooth transform vào bounds item, giữ việc preview độc lập
  với đường dẫn file gốc.
- Khi property đổi, phát signal và gọi `update()`.
- Không mở file ảnh, không sở hữu draft history và không nhúng ảnh vào PDF. Mục
  đích là bảo đảm preview/save dùng cùng pixel snapshot kể cả file nguồn bị đổi
  hoặc xóa sau khi người dùng chọn.

## Tài liệu, tab và render

### [`src/PDFDocument.cpp`](../src/PDFDocument.cpp) và
[`src/PDFDocument.h`](../src/PDFDocument.h)

**Vai trò:** model document trên GUI thread và chủ sở hữu main
`FPDF_DOCUMENT`.

**Mở/đóng:**

- Mở local file bằng `FPDF_LoadDocument()` từ path, không đọc toàn bộ PDF vào RAM.
- Quản lý PDFium process lifetime, password/error status và đóng handle đúng
  thread.
- Cache kích thước mọi source page để QML gọi `pageSize()` nóng mà không chạm
  PDFium lặp lại.
- Expose source URL, loaded/status, page count/current page, current/max page size.

**Cấu trúc trang transient:**

- Giữ page map hiển thị gồm source page và blank page chèn tạm; giữ danh sách
  source page đã đánh dấu xóa.
- `PDFPageInsertion` chứa vị trí/kích thước cần writer tái tạo khi Save.
- Cung cấp ánh xạ display page ↔ source page cho renderer, edit và form.
- Page insert/delete chỉ thay state session và revision; không sửa main document
  handle tại chỗ.

**Revision:** tăng khi document underlying hoặc cấu trúc render thay đổi. Mọi
worker result mang revision cũ phải bị loại.

**Ranh giới:** handle này chỉ dùng trên GUI thread. Worker lấy
`renderFilePath()` và tự mở handle riêng.

### [`src/PDFTabModel.cpp`](../src/PDFTabModel.cpp) và
[`src/PDFTabModel.h`](../src/PDFTabModel.h)

Tệp định nghĩa hai lớp có ownership khác nhau.

#### `PDFDocumentSession`

- Sở hữu `PDFDocument`, `PDFEditController`, `PDFFormController` và
  `PDFFormDesignerController` của một tab.
- Nối document vào ba controller, nối form/designer vào edit controller để draft
  guard và save snapshot bao quát cả content lẫn AcroForm.
- Giữ state view riêng: zoom, scroll X/Y, quarter-turn rotation, panel collapsed
  và cờ initial fit.
- Tổng hợp `hasDrafts`/`saving`, title/path và chuyển tiếp status, save finished,
  external-link request cho model/QML.
- `prepareForDeactivation()` kết thúc edit/focus form trước khi QML phá workspace.

#### `PDFTabModel`

- Là `QAbstractListModel` của session, expose title/session/draft/saving roles cho
  `DocumentTabBar`.
- `open()` chuẩn hóa canonical path và tái kích hoạt tab hiện có thay vì mở cùng
  tệp hai lần.
- Quản lý current index/session, đóng session, Save/Save As đúng tab và tìm tab
  đầu tiên còn draft cho workflow Quit.
- Chặn Save As ghi đè đường dẫn đang được tab khác dùng; xóa identity khi
  document đóng.
- Tổng hợp `anySaving` và `hasDrafts` toàn ứng dụng để `Main.qml` khóa hành động
  nguy hiểm.

**Ranh giới:** model không tự hiển thị prompt; nó trả trạng thái/signal để QML
điều phối lựa chọn người dùng.

### [`src/PDFPageItem.cpp`](../src/PDFPageItem.cpp) và
[`src/PDFPageItem.h`](../src/PDFPageItem.h)

**Type QML:** `PDFPageView`, kế thừa `QQuickItem`, bật `ItemHasContents`.

**Lập lịch render:**

- Theo dõi document, edit/form/designer controller, page, zoom, color và kích
  thước item/DPR.
- Tạo render key gồm request id, pointer document, render revision, display/source
  page, pixel size, zoom, DPR, draft/form/designer revision.
- Debounce thay đổi draft ngắn bằng timer để thao tác kéo không tạo hàng đợi render
  vô hạn; request cũ có atomic cancellation flag.
- Gửi việc qua hàng đợi render có giới hạn của `PDFFormWorker`, nhờ đó render và
  form cùng dùng session document tuần tự thay vì đụng handle.

**Render transaction:**

- Với blank page, chỉ tạo ảnh nền.
- Với source page, mở page qua form worker và giữ một `PDFiumProcessLock` cho toàn
  bộ render/mutate-tạm/restore.
- `ScopedPageObjectSuppression` ẩn source object đã bị draft thay/xóa.
- `ScopedPageObjectMoves` áp ma trận preview cho source text/image chỉ-move rồi
  phục hồi object.
- `PDFWidgetAnnotationSuppression` ẩn source widget đang được form designer vẽ
  hoặc Dropdown đang giữ tombstone xóa draft; capture nền vùng widget, cho
  `FPDF_FFLDraw` chạy để form khác vẫn đúng, rồi dán nền lại trên widget bị
  suppress.
- Chỉ tái sử dụng form document nếu mọi object transform, annotation flag và patch
  đều phục hồi thành công.

**Nhận kết quả và scene graph:**

- Callback giữ `QPointer<PDFPageItem>` và kiểm lại toàn bộ render key. Kết quả cũ
  chỉ bị bỏ, không thay texture hiện hành.
- `updatePaintNode()` tạo texture từ pending `QImage`, dùng
  `QSGSimpleTextureNode::setOwnsTexture(true)` và xóa bản CPU ngay sau upload.
- Không gọi `setTexture(nullptr)` và không delete thủ công texture đã được node
  sở hữu.
- Expose `draftTextureCurrent`/`draftTextureReady` để `EditOverlay` biết khi nào
  cần tự vẽ preview hoặc mask texture sạch.

**Ranh giới:** mọi thay đổi file này phải test native Metal, scroll/pool nhanh,
zoom, đóng/mở khi render đang chạy và shutdown render thread.

## Edit controller và writer nội dung

### [`src/PDFEditController.cpp`](../src/PDFEditController.cpp) và
[`src/PDFEditController.h`](../src/PDFEditController.h)

**Vai trò:** trung tâm nghiệp vụ của edit mode: khám phá nội dung, selection,
draft, rich text, history, page operation và orchestration Save.

**Khám phá candidate:**

- Scan page bất đồng bộ từ document handle riêng dưới PDFium lock.
- Duyệt top-level page objects và đường dẫn object con để nhận diện text/image;
  chỉ đánh dấu editable cho loại có thể lưu mà vẫn bảo toàn scope/resource.
- Gom các text object tương thích trên cùng visual line thành một candidate nhưng
  giữ mọi source object path để suppress/remove chính xác.
- Trích source bounds, baseline/origin, font family/style/size/stretch, render
  mode/color, character span và paragraph format.
- Cache candidate theo page/revision; hover có đường try-lock/defer để không khóa
  GUI khi PDFium đang bận.

**Draft model và history:**

- Draft chứa kind, source ids/paths, text, character spans, paragraph formats,
  immutable source rect, layout frame, content bounds, image snapshot, shape/
  highlight geometry và insertion order tùy loại.
- Mỗi action logic tạo snapshot command: add, delete, move, resize, format, page
  insert/delete. Undo/redo phục hồi toàn bộ state liên quan, không chỉ text.
- `hasDrafts` bao gồm edit history, transient page structure, form runtime draft
  và form designer changes.
- `discardDrafts()` phục hồi session state nhưng không ghi document.

**Text edit:**

- Quản lý selection range UTF-16, typing format và các signal để QML merge format
  vào `QTextDocument` đang chạy.
- Tạo frame ban đầu từ Qt text width; khi mở editor, tính frame mở rộng transient
  tới candidate cùng hàng kế tiếp hoặc margin suy ra.
- Phân biệt no-op editor session với thay đổi thật; frame mở rộng chỉ thành draft
  khi text/alignment/resize thay đổi.
- Font picker lấy intersection exact faces theo mọi style span đang chọn; đổi
  family reset inherited stretch cho range thay đổi.

**Công cụ khác:**

- Add Image decode/giới hạn ảnh trên GUI thread rồi lưu pixel snapshot.
- Add Shape và Highlight chuẩn hóa geometry, snap line/arrow gần trục và giữ màu.
- Move/delete top-level source text/image; bảo vệ object lồng trong Form XObject.
- Insert/delete page transient với điều kiện tài liệu luôn còn ít nhất một trang.

**Save orchestration:**

- Kết thúc editor, lấy form snapshot, gọi `PDFSaveWriter::prepare()` trên GUI
  thread rồi chạy `save(request)` trên worker.
- Đặt `saving` để khóa edit/tab operations, chuyển error thành status dịch được.
- Sau commit thành công, mở lại path; chỉ khi reopen thành công mới xóa draft và
  history. Nếu reopen thất bại, đóng document stale vì object path không còn khớp.

### [`src/PDFSaveWriter.cpp`](../src/PDFSaveWriter.cpp) và
[`src/PDFSaveWriter.h`](../src/PDFSaveWriter.h)

**Vai trò:** serializer giao dịch từ snapshot của controller sang PDF đích.

**API:**

- `prepare(controller, target, optionalSourceOverride)` tạo `Request` bất biến.
- `save(request)` chạy không cần controller; overload `save(controller, target)`
  dành cho call đồng bộ/test.
- `Result` trả enum lỗi, chi tiết, page và code point không hỗ trợ để controller
  tạo thông báo phù hợp.

**Snapshot chuẩn bị ở GUI thread:**

- Sao chép source/target path, deleted pages, page insertions, mọi draft và form
  designer save fields.
- Snapshot `QImage`, font bytes và rich-text format; worker không mở lại đường dẫn
  ảnh hay gọi CoreText/QML object.

**Áp nội dung:**

- Mở writer document riêng từ source path và resolve toàn bộ source object path
  trước khi remove bất kỳ object nào.
- Thực hiện delete/insert page, giữ mapping từ display page về source page.
- Với source text/image chỉ đổi vị trí, dịch ma trận object gốc để giữ font/pixel,
  orientation và z-order.
- Với text đổi nội dung/format/frame, remove source objects và reflow bằng
  `QTextLayout`; tạo object theo line/style run, map top-left Qt sang bottom-left
  PDF và vẽ underline bằng path.
- Reuse source font hoặc PDF standard font khi an toàn; nếu cần thì load bundled/
  system TrueType snapshot, giữ font handle sống đến sau GenerateContent.
- Bảo toàn exact source horizontal scale khi reuse; chỉ tính width compensation
  cho font mới/standard.
- Tạo bitmap object cho ảnh thêm mới, path object cho sáu shape, và standard
  Highlight annotation với quad/color/opacity/Print flag.
- Giữ earliest source z-order bằng insert-at-index; object thêm mới theo insertion
  order. Dùng `FPDF_DeviceToPage()` để tôn trọng CropBox/rotation.
- Chỉ gọi `FPDFPage_GenerateContent()` cho page có content object thay đổi;
  highlight-only page không bị rewrite content stream.
- Với source paint mà PDFium không thể tái tạo an toàn, chuẩn bị rasterized paint
  có giới hạn; các mode/scope unsupported phải trả lỗi thay vì làm mất đồ họa.

**Commit và form:**

- Ghi intermediate/content document qua `QSaveFile` với
  `FPDF_NO_INCREMENTAL | FPDF_SUBSET_NEW_FONTS`.
- Chuyển normalized designer field sang page coordinates và gọi
  `PDFAcroFormWriter` nếu có field cần ghi/normalize.
- Chỉ commit đích khi từng bước thành công; Save As không chạm source.

## Form runtime

### [`src/PDFFormController.cpp`](../src/PDFFormController.cpp) và
[`src/PDFFormController.h`](../src/PDFFormController.h)

**Vai trò:** facade form trên GUI thread; đây là object session mà QML và
`PDFFormView` được phép gọi.

**State expose:** interactive, has drafts/focus, accepts text input, focused page
và annotation, can undo/redo, cursor type, JavaScript enabled/pending.

**Gửi việc sang worker:**

- Tạo `PDFFormWorker::DocumentKey` từ session id, render path và revision.
- Chuyển pointer press/move/release/double click, key event, clipboard/edit
  command, page change, choice focus/selection và snapshot sang worker.
- Mỗi callback kiểm generation/key rồi mới cập nhật focus/cursor/history/render
  revision hoặc phát host action.

**PDF actions và sandbox:**

- Bật JavaScript form tự động và xử lý lifecycle/page action qua worker.
- Cho phép internal go-to điều hướng trong document.
- HTTP/HTTPS do tương tác người dùng tạo được chuyển thành external-link request
  để QML xác nhận; scheme/file/mail/print/submit/dialog/timer không hỗ trợ bị chặn.
- `confirmExternalLink()`/`cancelExternalLink()` dùng request id để không xác nhận
  nhầm popup cũ.

**Draft/save:**

- Theo dõi form value change như draft của tab; PDFium cung cấp undo/redo cho
  editable field nhưng không cho choice field không editable.
- Export form snapshot bất đồng bộ hoặc blocking vào file tạm cho save pipeline.
- Reset/destroy worker session khi document/revision thay đổi hoặc controller bị
  hủy.

### [`src/PDFFormWorker.cpp`](../src/PDFFormWorker.cpp) và
[`src/PDFFormWorker.h`](../src/PDFFormWorker.h)

**Vai trò:** toàn bộ form session PDFium ở phía worker và hàng đợi render/form
tuần tự.

**Registry và ownership:**

- Registry theo `sessionId` giữ document path/revision, `FPDF_DOCUMENT`,
  `FPDF_FORMHANDLE`, form-fill callbacks, JavaScript platform và active page.
- Mở document riêng từ path; không nhận main document handle từ GUI.
- Tất cả public submission API lên một queue có serialization; transaction giữ
  process-wide PDFium lock ở outer boundary.

**Interaction:**

- Map Qt pointer/key/modifier sang PDFium form APIs, duy trì focus, caret,
  selection, cursor và invalidation state.
- Thực thi document/page/open/close/action hooks và thu internal go-to/external
  URI từ callback host bị sandbox.
- Page change đóng/mở form page đúng thứ tự; reset session hủy state cũ.

**Choice-field safety:**

- Inspect visible Widget annotation và chỉ trả custom `ChoiceField` cho fixed
  combo/list có geometry/options bounded, không editable, không commit-on-select
  và không có `/A`/`/AA` action nguy hiểm cho semantic thay thế.
- Tạo identity token từ field/widget state. Trước focus/mutate luôn resolve lại
  source page và annotation rồi so token; không tin annotation index cũ.
- Multi-select toggle giải quyết trên live worker state, không trên QML snapshot.

**Designer/save support:**

- Khám phá field do TrePDF tạo, trả id/name/tooltip/value/default/type/flags/
  appearance version/source page/normalized rect cho designer rehydrate.
- Export snapshot của form session để giá trị native/JavaScript calculation được
  đưa vào nguồn writer.
- `withPageLocked()` cho renderer dùng preview/form page cùng session dưới lock.

**Callback contract:** worker trả plain structs (`InteractionResult`,
`SnapshotResult`, `ChoiceFieldsResult`, ...); không giữ QObject GUI trong request.

### [`src/PDFFormView.cpp`](../src/PDFFormView.cpp) và
[`src/PDFFormView.h`](../src/PDFFormView.h)

**Type QML:** `PDFFormView`, kế thừa `QQuickItem` và là cầu input native.

- Nhận controller, form designer controller, form design mode, display page và
  interactive flag.
- Bật hover/mouse/focus/input method; chuyển tọa độ item sang normalized/device
  geometry mà form controller cần.
- Chuyển key press/release, shortcut clipboard, IME preedit/commit và focus out;
  cập nhật cursor Qt từ `cursorType` của controller.
- Expose `choiceFields` và invokable focus/select để `FormChoiceOverlay` dùng.
  Refresh bất đồng bộ khi document/page/generation/render state thay đổi và loại
  response stale.
- Khi form designer đang phủ một source-backed TrePDF field hoặc giữ tombstone
  xóa Text Field/Dropdown/Radio, chặn native pointer path tại vùng đó để một
  click không vừa tác động designer draft vừa sửa widget PDFium bên dưới.
- Khi inactive/pooled/controller đổi, clear focus, choice snapshot và pending
  refresh.

## Form designer và ghi AcroForm

### [`src/PDFFormDesignerController.cpp`](../src/PDFFormDesignerController.cpp)
và [`src/PDFFormDesignerController.h`](../src/PDFFormDesignerController.h)

**Vai trò:** `QAbstractListModel` cho field đang thiết kế và field TrePDF đã
rehydrate.

**Model:**

- Field có id, name, tooltip, value/default value, type, flags, giới hạn ký tự,
  display/source page, normalized rect, source-backed, appearance version và
  bitmask property đã thay đổi.
- Enum model có Text, Checkbox, RadioButton, Dropdown, Date, Signature; UI và
  writer hoàn chỉnh hiện hỗ trợ Text/Checkbox/RadioButton/Dropdown.
- Expose roles cho `FormFieldLayer` và các API `fieldData()`/`fieldsForPage()` cho
  QML.

**Thao tác:**

- Tạo field với tên duy nhất, default size/value/flags; helper riêng cho Text và
  Comb Field. Comb vẫn là type Text, mặc định năm ô.
- Đổi value/name/options/flags/max length/rect; `moveField()` chỉ đổi top-left,
  giữ width/height và clamp field vào trang. Text Field không multi-line được
  chuẩn hóa về một dòng; max length dương được áp ngay lên value. Flag Comb yêu
  cầu max length dương, loại trừ Multiline, và khi đặt max length về `0` thì
  controller tự gỡ Comb.
- Chọn field, quản lý text focus, activate source checkbox/radio, chọn giá trị
  Dropdown, remove field transient và tạo tombstone khi xóa Text Field,
  Dropdown hoặc Radio source-backed do TrePDF tạo. `name` của Radio là Group ID;
  đổi tên gộp/tách nhóm và chọn một member sẽ tắt các member khác cùng tên.
- Map field qua page insertion/deletion; source-backed field giữ source page
  identity để update hoặc remove đúng widget sau save.

**Rehydrate:**

- `prepareForDesignMode()` yêu cầu `PDFFormWorker` inspect TrePDF fields.
- Kiểm generation/document key trước khi thay model; đồng bộ appearance version
  để writer biết field nào chỉ cần normalize appearance.
- Dropdown giữ annotation index và identity token; đổi lựa chọn source-backed đi
  qua form worker rồi refresh model thay vì tạo một value draft tách khỏi PDFium.
- Text field có `/Ff` Comb hợp lệ cùng `/MaxLen` được map trở lại flag designer,
  nên cell count và appearance tiếp tục chỉnh được sau reopen.

**Save snapshot:**

- `saveFields()` trả field add/update/normalize hợp lệ và tombstone Text
  Field/Dropdown/Radio dưới dạng `Remove`; field còn lại kèm
  `changedProperties` để writer không ghi đè value native nếu người dùng không
  sửa value trong designer.
- Draft state của model tham gia guard chung qua edit controller/session.
- Discard/reset xóa tombstone rồi rehydrate field nguồn từ document.

### [`src/FormFieldLayer.cpp`](../src/FormFieldLayer.cpp) và
[`src/FormFieldLayer.h`](../src/FormFieldLayer.h)

**Type QML:** `FormFieldLayer`, `QQuickItem` có scene graph và pointer events.

**Render:**

- Lấy field model theo display page, visibility/source visibility và pooled state.
- Dựng scene graph nodes cho Text Field, Check Box, Radio Button, Dropdown và
  presentation của các type dự trù; vẽ border, background, text/check mark/radio
  dot/arrow và selected state. Với Text Field có flag Comb, layer chia đều rect
  theo max length và căn ký tự giữa từng ô.
- Dùng constants từ `PDFFormAppearance.h` để preview font, border và padding khớp
  appearance writer.
- Expose active field id/rect/value/options/type/flags/source-backed để QML đặt
  editor, frame và handles đúng vị trí.

**Pointer interaction:**

- Hit-test field từ trên xuống, chọn field và phát `fieldActivated`; checkbox và
  radio có đường activate qua form runtime khi source-backed.
- Khi `moveEnabled`, hover đặt `SizeAllCursor`; press giữ mouse grab, qua
  `startDragDistance` mới bắt đầu move để click thuần vẫn activate.
- Move preview chỉ sửa `RenderField` cục bộ; `finishFieldMove()` gọi
  `controller->moveField()` đúng một lần, còn cancel phục hồi model.
- Có invokable begin/update/finish/cancel để move handle và keyboard trong QML
  dùng cùng pipeline.
- Reset pointer/move khi controller, page, pooling, mode, visibility hoặc
  interactivity đổi.

**Ranh giới scene graph:** chỉ tạo/sửa node trong `updatePaintNode()`; QObject/
model state được snapshot thành dữ liệu render ở GUI thread.

### [`src/PDFAcroFormWriter.cpp`](../src/PDFAcroFormWriter.cpp) và
[`src/PDFAcroFormWriter.h`](../src/PDFAcroFormWriter.h)

**Vai trò:** coordinator cấp cao ghi các field designer vào một PDF intermediate.

**Input:** source file path, target path và vector `Field` chứa widget id/name/
tooltip/value/default/options/rect/page/type/flags/operation.

**Pipeline:**

1. Yêu cầu caller đang giữ PDFium process lock và mở intermediate bằng PDFium.
2. Với update không đổi value/options, đọc live data từ document để tránh
   designer snapshot cũ ghi đè thay đổi form runtime/JavaScript.
3. Chuẩn hóa Checkbox/Radio Button về `Yes`/`Off`; kiểm Dropdown
   options/selection và chuyển Required/ReadOnly/Radio/Combo/Comb thành form
   flags. Text Comb còn ghi `DoNotScroll`, yêu cầu `/MaxLen` dương và không cho
   đồng thời Multiline. Radio không đặt `NoToggleToOff` để PDF reader cho phép
   bỏ chọn. Với Remove, chỉ chấp nhận Text Field, Dropdown hoặc Radio
   source-backed có widget id hợp lệ.
4. Với field Add, tạo Square annotation placeholder có rect, `NM=widgetId` và
   Print flag. PDFium public API chưa tạo đầy đủ Widget dictionary cần thiết.
5. Save bản có placeholder qua `QSaveFile` và PDFium.
6. Gọi `PDFAcroFormCanonicalizer::finalizeFields()` để biến placeholder/update
   thành field/widget chuẩn, tạo appearance hoặc gỡ field/Widget cần remove.
   Mỗi Group ID Radio Button được ghi đúng một field cha trong
   `/AcroForm/Fields`; các Widget con có `/Parent` và export value riêng để lựa
   chọn loại trừ hoạt động ổn định trên các PDF reader bên ngoài.
7. Mở lại file bằng PDFium, duyệt widget và kiểm field add/update khớp đúng một
   lần, còn field Remove không tồn tại.

**Khả năng hiện tại:** Text, Checkbox, grouped Radio Button và fixed
single-select Dropdown hỗ trợ Add/Update; Text và Radio còn hỗ trợ
NormalizeAppearance. Remove hỗ trợ top-level, single-widget Text Field/Dropdown
và member Radio thuộc group do TrePDF tạo; group Radio còn lại được dựng lại
khi xóa. Date/Signature, Remove Checkbox và cấu trúc AcroForm ngoài phạm vi
TrePDF phải fail rõ ràng.

**Failure:** nếu canonicalize hoặc validation thất bại, xóa target intermediate
để caller không dùng một PDF form nửa hoàn chỉnh.

### [`src/PDFAcroFormCanonicalizer.cpp`](../src/PDFAcroFormCanonicalizer.cpp)
và [`src/PDFAcroFormCanonicalizer.h`](../src/PDFAcroFormCanonicalizer.h)

**Vai trò:** bộ đọc/ghi PDF mục đích hẹp để hoàn thiện AcroForm mà PDFium public
API không thể dựng trực tiếp.

**Đọc cấu trúc:**

- Memory-map file, parse trailer/xref/object reference/dictionary/string đủ cho
  trường hợp AcroForm cần xử lý.
- Tìm Catalog, `/AcroForm`, `/Fields`, page annotation arrays, placeholder có
  `NM` khớp widget id và field/widget hiện có cần update/remove.
- Thu field names để phát hiện trùng tên và bảo toàn reference hiện hữu.
- Đây không phải PDF editor tổng quát; parser chỉ nên mở rộng khi writer có test
  fixture tương ứng.

**Sinh object:**

- Tạo/cập nhật Text Field Widget dictionary, field flags, `/MaxLen`, `/DA`,
  `/DR`, value và default value. Comb Field dùng `/FT /Tx`, cờ Comb chuẩn PDF và
  một cell cho mỗi ký tự theo `/MaxLen`.
- Render appearance text field thành image XObject có giới hạn tổng pixel, rồi
  tạo appearance stream để PDF reader hiển thị ổn định kể cả trước khi focus;
  field multi-line được wrap và căn trên trong appearance; field comb vẽ divider
  và căn từng ký tự giữa cell.
- Tạo Checkbox Widget với `/AP` Off/Yes riêng, `/AS`, value/default value và
  vector check appearance.
- Gom Radio theo Group ID thành một field cha `/FT /Btn` có `/Kids`; mỗi Widget
  con giữ `/Parent`, export value riêng theo widget id và appearance Off/On.
- Tạo Dropdown Widget `/FT /Ch` với Combo flag, `/Opt`, `/V`, `/DV`, `/I`, `/DA`
  và appearance có nút mũi tên.
- Chỉ cập nhật Dropdown có `/Opt` là mảng string trực tiếp; export/display pairs
  bị từ chối để không làm mất export value khi canonicalize.
- Với Remove Dropdown, chỉ nhận TrePDF field top-level có đúng một Widget trên
  page yêu cầu. Với Remove Radio, yêu cầu snapshot đủ group nguồn và dựng lại
  field cha từ các member còn lại. Cả hai cập nhật page `/Annots` và
  `/AcroForm /Fields` để bỏ reference tương ứng.
- Với normalize-only text field, giữ field data hiện hữu và chỉ thay appearance
  dictionary/version cần thiết.
- Bổ sung AcroForm vào Catalog nếu chưa có, cập nhật `/Fields`, default font
  resource và appearance version của TrePDF.

**Ghi:** không rewrite toàn file lần nữa; append incremental objects, xref và
trailer có `/Prev` trỏ xref cũ. Mọi giới hạn, reference mismatch hoặc parse lỗi
phải dừng trước khi append.

### [`src/PDFWidgetAnnotationSuppression.cpp`](../src/PDFWidgetAnnotationSuppression.cpp)
và [`src/PDFWidgetAnnotationSuppression.h`](../src/PDFWidgetAnnotationSuppression.h)

**Vai trò:** tạm ẩn Widget annotation cụ thể trong lúc render trang có form
designer, rồi phục hồi document và pixel nền an toàn.

- `suppress(page, widgetIds)` tìm Widget theo `/NM`, lưu annotation index/flags/
  page rect và thêm Hidden flag. Nếu thiếu bất kỳ id hoặc set flag lỗi, phục hồi
  ngay và báo false.
- Guard RAII gọi `restore()` trong destructor; caller phải giữ page mở và
  PDFium lock trong toàn bộ lifetime.
- `pageRects()` trả vùng cần patch.
- `capturePDFWidgetRenderPatches()` map bốn góc page rect sang device pixel và
  sao chép nền sau base render. Vùng được nới 4 px để bao cả dotted focus outline
  mà `FPDF_FFLDraw` có thể vẽ ngoài annotation rect.
- Sau khi restore Widget flags và gọi `FPDF_FFLDraw` cho toàn form,
  `restorePDFWidgetRenderPatches()` dán các vùng nền trở lại để field đang được
  designer vẽ không xuất hiện hai lần.
- Mọi patch kiểm format 32-bit, bounds và kích thước trước `memcpy`.

## Header hỗ trợ không có `.cpp`

### [`src/PDFFormAppearance.h`](../src/PDFFormAppearance.h)

- Chứa constants version cho appearance do TrePDF sinh, tách khỏi schema dữ liệu
  field để có thể migrate giao diện mà không mất validation properties.
- Chứa font size, border width/color và padding chuẩn của Text Field; cả
  `FormFieldLayer` và canonicalizer/writer phải dùng cùng giá trị.
- Text/checkbox/radio/dropdown có appearance version riêng; designer dùng version
  tương ứng để phát hiện field cũ cần normalize khi mở lại.

### [`src/PDFiumSynchronization.h`](../src/PDFiumSynchronization.h)

- Cấp một `QMutex` sống tới process exit vì destructor của thread-local PDFium
  document có thể chạy muộn trong lúc thread pool teardown.
- `PDFiumProcessLock` là RAII guard có Blocking/TryLock, thread-affine và cấm
  copy/move.
- Dùng thread-local flag để phát hiện lock lồng nhau và `qFatal` thay vì deadlock.
- `assertPDFiumProcessLockHeld()`/`NotHeld()` ghi rõ contract của helper.
- Không giữ lock khi emit signal, post callback hoặc chờ worker; không để helper
  con tự acquire lại lock do outer transaction đã giữ.

## Bài kiểm thử

### [`tests/tst_PDFEditController.cpp`](../tests/tst_PDFEditController.cpp)

**Vai trò:** bộ Qt Test tích hợp/hồi quy chính cho edit, save, form, font,
render concurrency và multi-tab. Tệp lớn vì tự dựng nhiều PDF fixture nhỏ để
kiểm round-trip bằng PDFium thay vì phụ thuộc tài liệu ngoài repository.

**Nhóm coverage hiện tại:**

- Form designer: lưu/rehydrate Text Field, Comb Field, Checkbox, grouped Radio
  Button và Dropdown; giữ lựa chọn/options khi move-resize; kiểm mutual
  exclusion, Group ID, max length/comb constraints, export/control index và xóa
  member Radio; xóa draft/Discard/Save round-trip cho TrePDF Dropdown;
  appearance normalization, AcroForm sẵn có và tên field trùng.
- Form runtime: JavaScript tự bật, choice field tùy biến và persistence, push
  button action, URI sandbox, external-link routing và PDFium serialization
  giữa thread.
- Candidate/hover: scan ngoài GUI thread, cancellation/reschedule/defer khi lock
  bận, grouping theo dòng/cột/style/font và giới hạn editable scope.
- Font: bundled/system registration, language filters, mixed-style intersection,
  embedding rejection, glyph coverage, TTC/subset collision và stretch
  calibration.
- Rich text: frame mở rộng, column boundary, resize/content bounds, selection
  formatting, typing format, paragraph alignment, editor undo và scale qua nhiều
  lần save.
- Content round-trip: text, Vietnamese, paint Fill/Stroke/pattern/grayscale,
  highlight, image, shape, source move/delete, z-order, CropBox và rotated page.
- Page operation: insert/delete, undo/redo/discard, source page mapping và save.
- Save lifecycle: async save, failure giữ draft, Save/Save As và reopen.
- Multi-tab: document/session độc lập, rotation/draft riêng, save đúng tab và
  chặn Save As đè tab khác.

**Cách thêm regression:** tạo fixture nhỏ nhất tái hiện lỗi, kiểm cả state trước
save và PDF mở lại sau save. Với thay đổi renderer/scene graph, test này không
thay thế kiểm thử native Metal thủ công.

## Ánh xạ tệp triển khai và header

| Triển khai | Header/API công khai |
|---|---|
| `ApplicationFonts.cpp` | `ApplicationFonts.h` |
| `DraftImageItem.cpp` | `DraftImageItem.h` |
| `FormFieldLayer.cpp` | `FormFieldLayer.h` |
| `PDFAcroFormCanonicalizer.cpp` | `PDFAcroFormCanonicalizer.h` |
| `PDFAcroFormWriter.cpp` | `PDFAcroFormWriter.h` |
| `PDFDocument.cpp` | `PDFDocument.h` |
| `PDFEditController.cpp` | `PDFEditController.h` |
| `PDFFormController.cpp` | `PDFFormController.h` |
| `PDFFormDesignerController.cpp` | `PDFFormDesignerController.h` |
| `PDFFormView.cpp` | `PDFFormView.h` |
| `PDFFormWorker.cpp` | `PDFFormWorker.h` |
| `PDFPageItem.cpp` | `PDFPageItem.h` |
| `PDFSaveWriter.cpp` | `PDFSaveWriter.h` |
| `PDFTabModel.cpp` | `PDFTabModel.h` |
| `PDFWidgetAnnotationSuppression.cpp` | `PDFWidgetAnnotationSuppression.h` |
| `main.cpp` | Không có header riêng |

## Checklist khi sửa C++

- Xác định object/thread nào sở hữu dữ liệu và handle; không capture QObject thô
  vào worker nếu lifetime không được guard bằng `QPointer`.
- Với PDFium, đặt một lock duy nhất ở outer transaction và bảo đảm mọi handle/
  mutation tạm được restore/destroy trước khi unlock.
- Với async result, so toàn bộ identity/generation/revision chứ không chỉ page
  index.
- Với save, snapshot dữ liệu GUI-thread trước; worker không truy cập controller,
  QML, CoreText live hoặc đường dẫn ảnh ban đầu.
- Với source object, resolve tất cả path trước remove; ưu tiên matrix-only cho move
  để giữ resource và z-order.
- Với form designer, test add, update, normalize, rehydrate, remove Dropdown và
  validation sau mở lại; enum tồn tại không đồng nghĩa writer đã hỗ trợ type đó.
- Chạy build `trepdf`, `git diff --check` và test liên quan. Thay đổi render/lifecycle
  phải thử thêm trên Metal với scroll, zoom, close/reopen và quit.
