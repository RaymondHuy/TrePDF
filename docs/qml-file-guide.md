# Hướng dẫn toàn bộ tệp QML

Tài liệu này mô tả 23 tệp QML đang được đăng ký trong QML module `TrePDF`.
Mỗi mục tập trung vào trách nhiệm, dữ liệu đi vào/đi ra, quan hệ với C++ và các
ràng buộc vòng đời cần giữ khi sửa code.

## Quy ước chung của lớp QML

- `Main.qml` là composition root. Component con phát signal ý định; cửa sổ hoặc
  workspace quyết định gọi controller nào.
- Dữ liệu PDF nghiệp vụ nằm trong C++. QML giữ state trình bày ngắn hạn như popup
  đang mở, gesture đang kéo, editor đang focus và delegate đang bị pool.
- Tọa độ draft/form truyền qua controller chủ yếu là tọa độ chuẩn hóa `0..1`
  theo trang chưa xoay. Xoay view chỉ nằm trên container Qt Quick dùng chung.
- `DocumentView` dùng `ListView.reuseItems`; mọi component trong delegate phải
  xem `pooled` như trạng thái ngắt hoạt động, hủy gesture/focus và không giữ
  document/render request cũ.
- Các chuỗi nhìn thấy bởi người dùng dùng `qsTrId()` với comment nguồn `//%`.
- Imports không ghi version; control tùy biến dùng `QtQuick.Controls.Basic`.

## Cây composition chính

```text
Main.qml
├── AppRibbon.qml
│   ├── ViewRibbon.qml
│   ├── EditRibbon.qml
│   ├── FormRibbon.qml
│   └── SignRibbon.qml
├── DocumentTabBar.qml
└── DocumentWorkspace.qml
    ├── DocumentView.qml
    │   └── PdfPageDelegate.qml (mỗi trang đang hiện)
    │       ├── PDFPageView (C++)
    │       └── PageInteractionOverlay.qml
    │           ├── PDFFormView (C++)
    │           │   └── FormChoiceOverlay.qml
    │           ├── EditOverlay.qml
    │           │   └── ShapeDraftView.qml
    │           └── FormTextFieldOverlay.qml
    │               └── FormFieldLayer (C++)
    ├── TextFormatPanel.qml
    ├── TextFieldOptionsPanel.qml
    ├── DropdownOptionsPanel.qml
    └── RadioButtonOptionsPanel.qml
```

Phần cấu trúc chồng lớp của một `PdfPageDelegate` được giải thích chi tiết hơn
trong [overlay-architecture.md](overlay-architecture.md): z-order, hệ tọa độ,
mode gate, pointer/focus routing, texture synchronization và pooling.

## Vỏ ứng dụng và ribbon

### [`src/Main.qml`](../src/Main.qml)

**Vai trò:** `ApplicationWindow` cấp cao nhất và nơi điều phối các hành động có
phạm vi toàn ứng dụng.

**Sở hữu và liên kết:**

- Tạo `AppTheme`, `PDFTabModel` và bốn object rỗng làm fallback khi chưa có tab:
  document, edit controller, form controller và form designer controller. Nhờ
  vậy ribbon vẫn có object hợp lệ để bind trong empty state.
- Theo dõi `activeSession`, `activeDocument`, `activeEditController` và
  `activeFormDesignerController` từ `tabModel.currentSession`.
- Tạo `DocumentWorkspace` bằng `Loader` chỉ cho session đang active. Hàm
  `prepareActiveWorkspace()` kết thúc editor/gesture trước khi đổi tab;
  `rebuildWorkspace()` phá và tạo lại workspace để tab không active không giữ
  page delegate hoặc texture.

**Điều phối hành động:**

- Mở một hoặc nhiều PDF; chuyển sang tab đã mở nếu model phát hiện cùng canonical
  path.
- Save, Save As, đóng tab, đóng cửa sổ và chuỗi xử lý draft chưa lưu. State
  `pendingDraftAction`/`pendingSession` ghi nhớ hành động cần tiếp tục sau khi
  người dùng Save, Discard hoặc Cancel.
- Hộp chọn ảnh lưu controller, page và điểm đặt tại thời điểm request; sau khi
  chọn file mới gọi `addImage()` để tránh áp ảnh vào tab khác nếu UI đổi state.
- Xác nhận xóa trang và kiểm tra đúng session/page trước khi thực hiện.
- Xác nhận liên kết HTTP/HTTPS do form/JavaScript yêu cầu; hiển thị authority,
  URL và cảnh báo HTTP không mã hóa trước khi gọi controller xác nhận.
- Hiển thị trạng thái tạm thời bằng timer và nhận status/save/external-link signal
  từ session/model.

**Input toàn cục:**

- Khai báo shortcut cho Open, Save, Save As, Undo, Redo và các hành động phù hợp
  khác; trước thao tác sẽ kết thúc inline edit để state controller nhất quán.
- Chặn đổi/đóng tab và đóng cửa sổ trong lúc có save bất đồng bộ.

**Ràng buộc:** không lưu draft hoặc PDF data trong QML này. Các biến `pending*`
chỉ là state của workflow UI và phải được xóa khi popup đóng hoặc session mất.

### [`src/Components/AppTheme.qml`](../src/Components/AppTheme.qml)

**Vai trò:** singleton-like value object chứa bảng màu giao diện.

- Cung cấp họ màu cam Foxit (`foxitOrange`, dark/hover/pale/border), màu nền và
  chữ ribbon, cùng màu xanh riêng cho tab Sign.
- Không chứa logic và không tự đọc system palette. Mọi component nhận `theme`
  bằng `required property AppTheme` để tránh lặp literal màu.
- Khi thay màu ở đây cần kiểm cả normal, hover, pressed, disabled, focus border
  và độ tương phản chữ/icon ở các reusable control.

### [`src/Components/AppRibbon.qml`](../src/Components/AppRibbon.qml)

**Vai trò:** host của bốn tab View, Edit, Forms, Sign và cầu nối giữa ribbon con
với `Main.qml`.

**Đầu vào:** `theme`, `document`, `editController`; giữ lựa chọn tool Text Field,
Check Box, Radio Button và Dropdown của Forms.

**Việc chính:**

- Vẽ hàng tab ribbon, vùng lệnh tương ứng và trạng thái tên tài liệu/draft/save.
- Tính `viewTabActive`, `editTabActive`, `formsTabActive` và các cờ tool form để
  workspace biết overlay nào được phép hiện/tương tác.
- Khi rời Edit, đưa `PDFEditController.tool` về Browse; khi vào Edit, mặc định về
  Select để không giữ một placement/drawing tool từ phiên trước.
- Khi chọn Forms tool, bảo đảm Text Field, Check Box, Radio Button và Dropdown
  loại trừ nhau.
- Chuyển tiếp signal Open/Save/Save As, page, delete page, zoom, fit, rotate và
  tool placeholder lên cửa sổ.

**Ràng buộc:** ribbon chỉ phát ý định và cập nhật tool state; không trực tiếp mở
dialog, đổi tab hoặc ghi tệp.

### [`src/Components/ViewRibbon.qml`](../src/Components/ViewRibbon.qml)

**Vai trò:** nhóm lệnh đọc và điều hướng tài liệu.

- Nhận `document` và `editController` để enable/disable lệnh theo `loaded` và
  `saving`.
- Phát Open, Save, Save As; trang trước/sau hoặc số trang yêu cầu; Zoom Out/In,
  Fit Width và Rotate Left/Right.
- Hiển thị tool chưa triển khai bằng `toolRequested(toolName)` thay vì giả lập
  hành vi trong component.
- Dùng `RibbonGroup` và `RibbonButton`, nên logic enable/split button phải thống
  nhất với component dùng chung.

### [`src/Components/EditRibbon.qml`](../src/Components/EditRibbon.qml)

**Vai trò:** toàn bộ command surface cho edit nội dung và page operation.

**Tool và command:**

- Select, Add Text, Add Image.
- Highlight dạng split button: click phần chính dùng màu hiện tại; menu có bảng
  màu marker curated và system color picker. Màu được ghi vào
  `editController.highlightColor` với opacity marker cố định.
- Shapes dạng split button: Rectangle, Oval, Line, Arrow, Triangle, Diamond.
  Menu dùng `ShapeDraftView` để preview và cập nhật `shapeType` trước khi bật
  `AddShapeTool`.
- Undo, Redo, Discard dựa trên state history/saving của controller.
- Insert Blank Page và Delete Current Page; xóa phát signal để cửa sổ hiển thị
  xác nhận thay vì xóa ngay.

**Đầu ra:** đổi trực tiếp tool/config thuộc controller khi đó chỉ là state edit
nhẹ; phát `pageRequested`, `deletePageRequested`, `toolRequested` cho hành động
cần cấp cao hơn.

**Ràng buộc:** menu tool phải đóng sạch khi document/session thay đổi. Không tự
ghi Save; mọi thay đổi vẫn là draft.

### [`src/Components/FormRibbon.qml`](../src/Components/FormRibbon.qml)

**Vai trò:** ribbon tối giản cho form designer hiện hỗ trợ đặt field.

- Nhận document và bốn cờ lựa chọn từ `AppRibbon`.
- Phát signal tool cho Text Field, Check Box, Radio Button hoặc Dropdown.
- Chỉ enable khi document đã load.
- Date/Signature vẫn là điểm mở rộng; ribbon chỉ quảng bá bốn loại đã có
  pipeline lưu hoàn chỉnh.

### [`src/Components/SignRibbon.qml`](../src/Components/SignRibbon.qml)

**Vai trò:** presentation của tab Sign và điểm mở rộng cho tính năng ký.

- Nhận theme/document, dùng biến thể màu xanh của reusable ribbon controls.
- Phát `toolRequested(toolName)` cho các nút chưa có backend hoàn chỉnh.
- Không tạo chữ ký, không sửa PDF và không giữ credential/chứng thư.

## Tab và workspace

### [`src/Components/DocumentTabBar.qml`](../src/Components/DocumentTabBar.qml)

**Vai trò:** thanh tab ngang ánh xạ trực tiếp `PDFTabModel`.

- Delegate nhận `index` và `PDFDocumentSession`; hiển thị title, active state,
  dấu draft và trạng thái saving.
- Click tab phát `tabRequested(index)`; nút đóng phát chính object session để
  caller vẫn xác định đúng tab nếu index thay đổi; nút cộng phát `openRequested()`.
- Đồng bộ `currentIndex` với model và dùng vùng cuộn để bảo đảm tab active hiện
  trong viewport khi mở/chuyển tab.
- Không tự xử lý draft guard. `Main.qml` quyết định Save/Discard/Cancel trước khi
  gọi model đóng session.

### [`src/Components/DocumentWorkspace.qml`](../src/Components/DocumentWorkspace.qml)

**Vai trò:** composition dành riêng cho một `PDFDocumentSession` đang active.

**Kết nối state:**

- Bind `session.document`, edit/form/designer controller vào `DocumentView`.
- Hai chiều hóa zoom và vị trí scroll với `session.zoom`, `scrollX`, `scrollY`;
  truyền `session.viewRotation` và trạng thái tab/tool form.
- Thực hiện Fit Width từ kích thước viewport và maximum page dimension đã xoay;
  initial-fit chỉ chạy khi session còn `initialFitPending`.
- Load đúng một panel ở cạnh phải: `TextFormatPanel` khi Edit active,
  `TextFieldOptionsPanel` khi chọn Text Field, `DropdownOptionsPanel` khi chọn
  Dropdown, hoặc `RadioButtonOptionsPanel` khi chọn Radio Button trong Forms.
  Khi Text Field vừa được chọn, panel của nó tự mở; trạng thái collapsed sau đó
  vẫn được lưu riêng theo session.
- Chuyển các signal font family, bold/italic/underline, color và paragraph
  alignment vào `PDFEditController`.

**Vòng đời:** `prepareForDeactivation()` yêu cầu `DocumentView` kết thúc editor,
hủy interaction và lưu scroll trước khi workspace bị phá; hàm cũng chạy trong
`Component.onDestruction` để không phụ thuộc caller luôn nhớ gọi.

### [`src/Components/DocumentView.qml`](../src/Components/DocumentView.qml)

**Vai trò:** viewport trang liên tục và nơi quản lý virtualization/navigation.

**Cấu trúc và state:**

- `ListView` dọc có `reuseItems: true`, `clip: true`, cache buffer bảo thủ và
  model là `document.pageCount`.
- Delegate là `PdfPageDelegate`, nhận tất cả controller, zoom, rotation và cờ
  mode/tool.
- `currentPage` của document được suy ra từ trang nổi trội trong viewport;
  navigation theo lệnh dùng `pendingNavigationPage` và generation để tránh một
  callback scroll cũ ghi đè thao tác mới.
- Khôi phục `contentX/contentY`, expose viewport geometry cho workspace và fit.
- Khi vào Forms/Edit có source form fields, gọi chuẩn bị design mode một lần theo
  generation; theo dõi yêu cầu rollback page/focus từ form controller.
- Empty state cung cấp nút Open; toast trình bày `transientStatus`.

**API nội bộ quan trọng:** chuẩn bị session change, bắt đầu edit selected text,
đi tới trang, lưu/khôi phục vị trí và đồng bộ form design mode.

**Ràng buộc:** không tăng `cacheBuffer` tùy tiện; mỗi delegate có thể giữ texture
Retina đầy trang. Mọi state delegate phụ thuộc trang phải reset khi reused.

### [`src/Components/PdfPageDelegate.qml`](../src/Components/PdfPageDelegate.qml)

**Vai trò:** một trang trong `ListView`, ghép nội dung PDF với mọi lớp tương tác.

**Bố cục:**

- Tính kích thước trang từ `document.pageSize(index)`, zoom và quarter-turn.
- `pageSurface` dùng chung chứa `PDFPageView` và
  `PageInteractionOverlay`; coordinator này chứa runtime form, edit và form
  designer.
- Rotation 90° được áp lên chính container dùng chung này; do đó texture, hit
  target form và overlay edit luôn xoay cùng nhau, còn tọa độ lưu không đổi.
- `PDFPageView` nhận edit/form/designer controller để render texture đã phản ánh
  suppression và draft preview.
- Delegate chuyển controller, mode, texture revision và viewport transform vào
  `PageInteractionOverlay`; coordinator xử lý z-order và cleanup chung.
- Hiển thị số trang ngoài page surface.

Xem [overlay-architecture.md](overlay-architecture.md) để có cây item, z-order
và ma trận mode/input đầy đủ của các nhánh này.

**Pooling:** `ListView.onPooled` đặt `pooled = true`, khiến child ngắt document,
interaction và focus; `onReused` bật lại để request state của page mới. Đây là
invariant quan trọng nhất của tệp.

### [`src/Components/PageInteractionOverlay.qml`](../src/Components/PageInteractionOverlay.qml)

**Vai trò:** coordinator duy nhất cho interaction của một trang. Component này
không vẽ và không tạo layer/FBO; nó đặt `PDFFormView`, `EditOverlay` và
`FormTextFieldOverlay` vào cùng hệ tọa độ, tính điều kiện runtime form và chuyển
choice-field handshake tới đúng controller. `PDFPageView.formDesignMode` cũng
bind vào mode đã tính ở đây để suppression và interaction dùng cùng trạng thái.

**Điều hướng pointer:** form designer nằm trên Edit nhưng `FormFieldLayer` chỉ
hit bên trong rect field. Vùng trống vì vậy đi xuống Edit; Forms placement và
background selection được xử lý bởi các `MouseArea` riêng trong form overlay.

**API delegate:** `prepareForSessionChange()` gom cleanup của edit/designer;
`beginSelectedEditing()` chuyển tiếp vào `EditOverlay`.

## Overlay sửa nội dung

### [`src/Components/EditOverlay.qml`](../src/Components/EditOverlay.qml)

**Vai trò:** lớp QML chính cho chọn, tạo, sửa, di chuyển, resize và xóa nội dung
draft trên một trang. Đây là tệp QML phức tạp nhất dự án.

**Dữ liệu vào:** page, trạng thái pooled, hai cờ texture draft, theme và
`PDFEditController`. Component tải `candidatesForPage()`/`draftsForPage()` và
làm mới khi controller báo page data/candidate/history/selection đổi.

**Hit testing và con trỏ:**

- Chuyển pointer pixel sang tỉ lệ trang và gọi candidate cache của controller.
- Có hit-test riêng cho rectangle/oval/line/arrow/polygon với tolerance theo
  kích thước trang.
- Quản lý con trỏ Select/Add Text bằng SVG và fallback cursor; ẩn cursor khi
  pooled, ngoài trang hoặc tool không phù hợp.
- Các công cụ draw shape/highlight giữ start/current point tạm thời, validate
  gesture rồi mới tạo một draft command.

**Text rich-edit:**

- Chuyển character spans và paragraph formats thành rich HTML cho `TextEdit`;
  escape text/attribute/color và hiệu chỉnh font stretch hiển thị.
- Phân biệt source bounds bất biến, layout frame và laid-out content bounds.
- Khi mở source text, frame có thể mở rộng transient tới biên cột; chỉ controller
  quyết định khi nào geometry trở thành draft thật.
- Đồng bộ selection range, typing format và B/I/U với `QTextDocument` mà không
  thay toàn bộ document, nhờ đó local undo/redo của editor không bị mất.
- Commit/cancel editor, sync content bounds sau layout và chuyển focus an toàn
  khi session/tool/selection đổi.

**Draft presentation:**

- Mask source rect khỏi texture sạch khi cần, rồi vẽ text/image/shape/highlight
  draft lên trên. Nếu render worker đã trả texture draft đúng revision, component
  tránh chồng preview không cần thiết.
- `DraftImageView` vẽ pixel snapshot của ảnh thêm mới; `ShapeDraftView` vẽ vector.
- Hiển thị bounding box, hover box, contextual Edit/Delete và resize handles.
- Drag text/image/shape giữ preview trong QML nhưng chỉ commit geometry một lần
  khi thả; phím mũi tên tạo move command, Shift dùng bước lớn hơn.

**Dọn state:** `prepareForSessionChange()` kết thúc hoặc hủy editor/resize/draw,
ẩn cursor và xóa dữ liệu trang. Bất kỳ bổ sung gesture nào cũng phải đi qua
đường cleanup này và trạng thái `pooled`.

### [`src/Components/ShapeDraftView.qml`](../src/Components/ShapeDraftView.qml)

**Vai trò:** renderer vector dùng lại cho preview trong menu, gesture đang vẽ và
draft shape đã tạo.

- Input: `shapeType`, `startRatio`, `endRatio`, fill/stroke color và stroke width.
- Dùng `QtQuick.Shapes` để tạo Rectangle, Oval cubic Bézier, Line, Arrow,
  Triangle và Diamond.
- Tính inset theo stroke để nét không bị cắt ở biên; line/arrow tôn trọng hướng
  start/end, arrowhead tính từ vector đơn vị.
- Không hit-test, snap hoặc lưu PDF. Controller/`EditOverlay` làm các việc đó;
  `PDFSaveWriter` dựng lại hình học tương ứng khi serialize.

### [`src/Components/TextFormatPanel.qml`](../src/Components/TextFormatPanel.qml)

**Vai trò:** panel định dạng text có thể thu gọn ở cạnh phải workspace.

**Đầu vào:** danh sách font khả dụng, callback lấy font theo language filter,
font/style/color/alignment đang chọn và `formattingEnabled`.

**Chức năng:**

- Dropdown font có nhóm Vietnamese, English, Chinese, Japanese, Korean. Khi đổi
  filter, gọi function do workspace/controller cung cấp rồi vẫn giữ font nguồn
  hiện tại làm giá trị hiển thị nếu nó bị lọc khỏi danh sách chọn.
- Nút Bold, Italic, Underline, color picker và menu căn trái/phải/giữa/justify.
- Phát signal yêu cầu thay đổi; panel không tự sửa draft.
- Có UI font size nhưng pipeline hiện chưa nối size vào draft formatting; không
  mô tả control đó như tính năng lưu được cho tới khi backend được nối.
- `collapsedChangeRequested` cho phép session giữ trạng thái thu gọn độc lập.

**Ràng buộc:** style button phải phản ánh state controller sau local editor
undo/redo, không chỉ state click gần nhất trong panel.

### [`src/Components/TextFieldOptionsPanel.qml`](../src/Components/TextFieldOptionsPanel.qml)

**Vai trò:** panel bên phải cho Text Field đang chọn trong Forms.

- Bật/tắt flag multi-line mà không làm mất Required/ReadOnly hiện có.
- Cho bật giới hạn ký tự tùy chọn; `0` nghĩa là không giới hạn, còn SpinBox nhận
  giá trị từ 1 đến 1.000.000.
- Mọi thay đổi gọi trực tiếp `PDFFormDesignerController`; khi hạ giới hạn dưới
  độ dài hiện tại, controller cắt value ngay trong draft form.
- Nút xóa ở cuối panel gọi `removeField()` cho cả Text Field mới tạo và
  source-backed do TrePDF tạo. Xóa source-backed chỉ là draft cho tới Save;
  Discard sẽ rehydrate field.
- Nội dung panel nằm trong `Loader` và dùng trạng thái collapsed của session.

### [`src/Components/RadioButtonOptionsPanel.qml`](../src/Components/RadioButtonOptionsPanel.qml)

**Vai trò:** panel bên phải cho Radio Button đang chọn trong Forms.

- Đọc `field.name` làm Group ID; trim input và gọi `setFieldName()` khi kết thúc
  chỉnh sửa. Workspace cũng flush edit đang pending trước Save/Save As để lệnh
  lưu không chụp tên nhóm cũ khi ô nhập vẫn giữ focus. Group ID rỗng, có dấu
  chấm hoặc xung đột với field không phải Radio bị controller từ chối.
- Giải thích rõ các Radio cùng Group ID thuộc một nhóm và chỉ một member được
  chọn.
- Nút xóa ở cuối panel gọi `removeField()`, dùng được cho cả draft mới và Radio
  source-backed do TrePDF tạo; thay đổi chỉ ghi vào PDF khi Save/Save As.
- Nội dung panel nằm trong `Loader` để được hủy sạch khi thu gọn hoặc đổi field.

## Overlay form

### [`src/Components/FormChoiceOverlay.qml`](../src/Components/FormChoiceOverlay.qml)

**Vai trò:** presentation Qt Quick cho tập combo box/list box AcroForm đã được
worker xác nhận là an toàn để thay native UI.

**Dữ liệu field:** normalized rect, source page, annotation index, identity token,
accessible name, options, selected indices, read-only và multi-select. Overlay
không tự khám phá annotation; danh sách đến từ `PDFFormView`.

**Combo box:**

- Click/keyboard yêu cầu focus qua `fieldFocusRequested`; chỉ mở popup sau khi
  callback xác nhận đúng source page, annotation index và identity token.
- Quản lý option highlight, Enter/Escape, mũi tên, Home/End và type-ahead có
  timeout.
- Popup tính vùng nhìn thật qua mapping vào `viewportItem`, chọn mở lên hoặc
  xuống theo chỗ trống, clamp chiều rộng/chiều cao và vị trí vào viewport.

**List box:**

- Hiển thị selection đơn/đa, hỗ trợ keyboard và giữ scroll position theo identity
  của field.
- Phát `optionSelected(...)` kèm selected/toggle và token; worker xác thực lại
  live state trước khi mutate để tránh toggle nhanh dựa trên snapshot cũ.

**An toàn vòng đời:** đóng popup và xóa pending state khi page, document,
interactive state, field list hoặc viewport transform thay đổi. Field read-only
có thể hiển thị nhưng không nhận mutation.

### [`src/Components/FormTextFieldOverlay.qml`](../src/Components/FormTextFieldOverlay.qml)

**Vai trò:** lớp tương tác cho form designer, bao gồm field transient và field do
TrePDF rehydrate sau khi mở lại.

**Đặt field:**

- Khi Text Field, Check Box, Radio Button hoặc Dropdown tool active, MouseArea
  nền nhận click/drag.
- Click tạo kích thước mặc định; drag tạo rect chuẩn hóa. Check Box và Radio
  Button ép hình vuông; Text Field và Dropdown dùng hình chữ nhật và tôn trọng
  kích thước tối thiểu.
- Sau khi tạo Text Field, lưu `pendingFocusFieldId` để editor chỉ focus sau khi
  model và loader đã sẵn sàng.

**Hiển thị và tương tác:**

- `FormFieldLayer` C++ vẽ field bằng scene graph, cung cấp active field id/rect/
  value/type/flags/source-backed và xử lý click/toggle/drag move.
- `Loader` tạo `TextField` native cho field text active; đồng bộ value vào
  controller nhưng tránh vòng lặp trong lúc nạp editor. Field có flag multi-line
  dùng `TextArea`, wrap nội dung và vẫn đồng bộ qua cùng controller.
- `Loader` tạo `ComboBox` cho Dropdown active từ options trong model và cập nhật
  selected value transient qua designer controller; với source-backed field,
  lựa chọn được chuyển qua PDFium form session rồi refresh lại model.
- Active frame có bốn edge resize handles. Resize preview ở QML và commit qua
  `setFieldRect()` khi kết thúc; cancel phục hồi model.
- Field có thể kéo trực tiếp sau ngưỡng drag. Field text được chọn còn có move
  handle ở góc và hỗ trợ phím mũi tên; C++ clamp top-left để giữ nguyên kích
  thước trong trang rồi gọi `moveField()` đúng một lần khi kết thúc.
- Check Box và Radio Button có thể activate/toggle; chọn một Radio sẽ tắt member
  khác cùng Group ID. Dropdown có thể mở/chọn option. Text Field và Check Box
  transient có nút xóa nổi; Text Field, Dropdown và Radio Button transient hoặc
  source-backed do TrePDF tạo được xóa từ panel tùy chọn. Discard phục hồi, còn
  Save/Save As mới gỡ Widget source-backed. Source-backed Check Box và cấu trúc
  AcroForm ngoài phạm vi TrePDF vẫn được bảo vệ.

**Phối hợp mode:** field có thể visible trong Forms hoặc Edit, nhưng placement,
move, resize, editor và delete được gate riêng bằng `formsActive`, `editActive`,
`fieldsVisible`, `fieldsInteractive`, tool active và `pooled`.

**Dọn state:** session change, mất visibility/interactivity, đổi tool hoặc pooling
phải cancel placement, resize, move và focus. Không để loader giữ focus sang
delegate được reuse.

## Component trình bày dùng chung

### [`src/Components/RibbonButton.qml`](../src/Components/RibbonButton.qml)

**Vai trò:** `ToolButton` chuẩn của ribbon.

- Nhận icon, tooltip, cờ cần document, primary/signing, giữ màu icon và trạng
  thái menu indicator.
- Kết hợp `documentLoaded` với `requiresDocument` để gate enable mà caller không
  lặp điều kiện.
- Hỗ trợ split-button: vùng action chính vẫn kích hoạt `clicked`, vùng menu riêng
  phát `menuRequested()` và có tooltip riêng.
- Vẽ hover/pressed/checked/focus theo `AppTheme`; biến thể signing dùng màu xanh.
- Khi thêm hành vi mới, tránh để hit area menu nuốt phím Space/Enter của action
  chính hoặc phá accessible name của control.

### [`src/Components/RibbonGroup.qml`](../src/Components/RibbonGroup.qml)

**Vai trò:** container bố cục cho một nhóm lệnh ribbon.

- Cung cấp vùng content mặc định, tiêu đề nhóm ở dưới và separator dọc.
- Nhận theme/title; không chứa command logic.
- Giữ chiều cao/padding nhất quán giữa các ribbon con, nên thay geometry ở đây có
  tác động rộng.

### [`src/Components/RibbonTabButton.qml`](../src/Components/RibbonTabButton.qml)

**Vai trò:** nút chọn View/Edit/Forms/Sign.

- Tùy biến background, text color và indicator theo checked/hover/focus.
- Nhận theme, còn label/check state do `AppRibbon` cung cấp.
- Chỉ chịu trách nhiệm presentation; ButtonGroup/tab index nằm ở parent.

### [`src/Components/OrangeButton.qml`](../src/Components/OrangeButton.qml)

**Vai trò:** nút hành động màu cam cỡ gọn dùng ở empty state/dialog hoặc vùng
không cần toàn bộ semantics của ribbon.

- Nhận theme; tùy biến background/border/text theo hover, pressed, disabled và
  focus.
- Vẫn là `Button` chuẩn nên caller dùng `text`, `onClicked`, keyboard và
  accessibility như control Qt Quick thông thường.

## Quan hệ QML với type C++ đăng ký vào module

| Tên dùng trong QML | C++ | Nơi dùng chính |
|---|---|---|
| `PDFTabModel`, `PDFDocumentSession` | `PDFTabModel.cpp` | `Main.qml`, tab bar, workspace |
| `PDFDocument` | `PDFDocument.cpp` | ribbon, view, delegate |
| `PDFEditController` | `PDFEditController.cpp` | ribbon edit, overlay, panel format |
| `PDFFormController` | `PDFFormController.cpp` | page delegate và `PDFFormView` |
| `PDFFormDesignerController` | `PDFFormDesignerController.cpp` | form designer overlay/layer và panel Dropdown/Radio |
| `PDFPageView` | `PDFPageItem.cpp` | `PdfPageDelegate.qml` |
| `PDFFormView` | `PDFFormView.cpp` | `PageInteractionOverlay.qml` |
| `DraftImageView` | `DraftImageItem.cpp` | `EditOverlay.qml` |
| `FormFieldLayer` | `FormFieldLayer.cpp` | `FormTextFieldOverlay.qml` |

## Checklist khi sửa QML

- Kiểm tra component có nằm trong page delegate được pool hay không; nếu có,
  thêm cleanup cho mọi state/gesture/timer/focus mới.
- Giữ `PageInteractionOverlay` và page texture trong cùng `pageSurface`; thêm
  interaction mới vào coordinator nếu dùng cùng tọa độ và rotation.
- Không chuyển tọa độ view đã xoay ngược vào model/save; controller nhận normalized
  geometry của trang logic.
- Không giữ pointer controller/session trong dialog callback nếu tab có thể đổi;
  chụp đúng session/controller như `Main.qml` đang làm.
- Mọi text mới phải dùng `qsTrId()` literal, comment `//%`, cập nhật catalog và
  kiểm layout tiếng Việt.
- Sau thay đổi: chạy `trepdf_qmllint`, build ứng dụng và thử scroll nhanh để ép
  delegate pool/reuse.
