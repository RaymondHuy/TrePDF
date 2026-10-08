# Hướng dẫn toàn bộ tệp QML

Tài liệu này mô tả các tệp QML đang được đăng ký trong QML module `TrePDF`.
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
│   │   ├── PageDisplayControl.qml
│   │   └── SearchRibbonControl.qml
│   ├── EditRibbon.qml
│   ├── CommentRibbon.qml
│   ├── FormRibbon.qml
│   └── SignRibbon.qml
├── SignatureDialog.qml (Loader khi tạo chữ ký/ngày/dấu)
├── DocumentTabBar.qml
├── ReaderControlBar.qml (footer cố định)
└── DocumentWorkspace.qml
    ├── DocumentView.qml (khung chính và tùy chọn khung phụ)
    │   └── PageLayoutDelegate.qml (một hàng hoặc spread)
    │       ├── PdfPageDelegate.qml (trang thứ nhất)
    │       │   ├── PDFPageView (C++)
    │       │   ├── SearchHighlightLayer.qml
    │       │   └── PageInteractionOverlay.qml
    │       │       ├── PDFFormView (C++)
    │       │       │   └── FormChoiceOverlay.qml
    │       │       ├── EditOverlay.qml
    │       │       │   ├── ShapeDraftView.qml
    │       │       │   ├── SquigglyLine.qml
    │       │       │   ├── CommentDrawingView.qml
    │       │       │   ├── InkDrawingView.qml
    │       │       │   └── StampView.qml
    │       │       └── FormTextFieldOverlay.qml
    │       │           └── FormFieldLayer (C++)
    │       └── PdfPageDelegate.qml (trang thứ hai, khi Facing; cùng cấu trúc)
    ├── CommentPanel.qml
    ├── DrawingCommentPropertiesPanel.qml
    ├── ImagePropertiesPanel.qml
    ├── HeaderTextPanel.qml
    ├── HeaderTextPlacement.qml
    ├── HeaderTextOverlay.qml
    ├── TextFormatPanel.qml
    ├── TextFieldOptionsPanel.qml
    ├── CombFieldOptionsPanel.qml
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

- Tạo `AppTheme`, `PDFTabModel` và năm object rỗng làm fallback khi chưa có tab:
  document, edit controller, form controller, form designer controller và search
  controller. Nhờ vậy ribbon vẫn có object hợp lệ để bind trong empty state.
- Theo dõi `activeSession`, `activeDocument`, `activeEditController`,
  `activeFormDesignerController` và `activeSearchController` từ
  `tabModel.currentSession`.
- Tạo `DocumentWorkspace` bằng `Loader` chỉ cho session đang active. Hàm
  `prepareActiveWorkspace()` kết thúc editor/gesture trước khi đổi tab;
  `rebuildWorkspace()` phá và tạo lại workspace để tab không active không giữ
  page delegate hoặc texture.
- Navigation search chụp `resultsRevision`; workspace mới chỉ nhận target sau
  barrier restore/fit ban đầu, nên callback Loader hoặc query cũ không ghi đè
  scroll của session mới.
- Truyền session đang active vào ribbon và chuyển các intent Page Display thành
  thay đổi trên đúng `DocumentWorkspace`; khi Loader chưa sẵn sàng thì cập nhật
  state session để workspace mới nhận lại.
- Gắn `ReaderControlBar` làm footer cố định, bind trang/zoom theo pane đang active
  và chuyển điều hướng trực tiếp của thanh đáy tới đúng `DocumentView` đó.

**Điều phối hành động:**

- Mở một hoặc nhiều PDF từ file dialog, kéo thả hoặc singleton
  `FileOpenHandler`; chuyển sang tab đã mở nếu model phát hiện cùng canonical
  path. `pendingOpenFiles` giữ yêu cầu trong lúc lưu/hộp thoại; mở tiếp khi hết
  chặn, qua cùng luồng mật khẩu.
- Save, Save As, đóng tab, đóng cửa sổ và chuỗi xử lý draft chưa lưu. State
  `pendingDraftAction`/`pendingSession` ghi nhớ hành động cần tiếp tục sau khi
  người dùng Save, Discard hoặc Cancel.
- Hộp chọn ảnh lưu controller, page và điểm đặt tại thời điểm request; sau khi
  chọn file mới gọi `addImage()` để tránh áp ảnh vào tab khác nếu UI đổi state.
- Sở hữu `SignatureLibrary` dùng chung và tạo `SignatureDialog` bằng Loader.
  Preview chữ ký/ngày/dấu được giữ đến khi click đặt ảnh vào pane chính. Pending
  placement gắn với session và bị hủy khi đổi tab/session/document, lưu, đổi
  history/tool hoặc nhấn Escape; không đặt ảnh vào session khác.
- Xác nhận xóa trang và kiểm tra đúng session/page trước khi thực hiện.
- Xác nhận liên kết HTTP/HTTPS do form/JavaScript yêu cầu; hiển thị authority,
  URL và cảnh báo HTTP không mã hóa trước khi gọi controller xác nhận.
- Hiển thị trạng thái tạm thời bằng timer và nhận status/save/external-link signal
  từ session/model.

**Input toàn cục:**

- Cmd/Ctrl+O mở file dialog. Cmd/Ctrl+F commit editor/panel, chuyển ribbon sang
  View, cuộn ribbon để hiện Search và focus/chọn toàn bộ query hiện tại.
- Khai báo shortcut cho Save, Save As, Undo, Redo và các hành động phù hợp khác;
  trước thao tác sẽ kết thúc inline edit để state controller nhất quán.
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

**Vai trò:** host của chín tab View, Edit, Comment, Forms, Sign, Protect,
Convert, Header và Footer; cầu nối giữa ribbon con với `Main.qml`.
Header có nút Add Text dùng cùng icon `../assets/icons/type.svg`
và `qsTrId("AddText")` với Edit; phát `headerTextRequested()` để bật đặt chữ bằng click trong vùng Header.
Footer hiện có vùng lệnh trống. Khi Header active, mỗi trang đang hiển thị có
dải xám nhạt cao 100 PDF pt ở đầu trang.

**Đầu vào:** `theme`, `document`, `editController`, `searchController`; giữ lựa
chọn tool Text Field, Check Box, Radio Button, Dropdown, List Box và Comb Field
của Forms.

**Việc chính:**

- Vẽ hàng tab ribbon, vùng lệnh tương ứng và trạng thái tên tài liệu/draft/save.
- Tính `viewTabActive`, `editTabActive`, `commentTabActive`, `formsTabActive`, `signTabActive` và
  `protectTabActive`, `exportTabActive`, `headerTabActive`, `footerTabActive` cùng
  các cờ tool form để workspace biết overlay nào được phép hiện/tương tác.
- Khi vào Edit hoặc Sign, mặc định về `SelectTool`; khi vào Comment, bật
  `CommentSelectTool`; rời các tab này đưa `PDFEditController.tool` về Browse để
  không giữ một tool tương tác từ phiên trước.
- Khi Comment active, yêu cầu annotation-only comment index cho toàn tài liệu,
  kể cả khi panel danh sách đang được ẩn.
- `syncEditTool()` đọc trực tiếp `ribbonTabs.currentIndex` trong signal đổi tab,
  tránh dùng các binding `editTabActive`/`commentTabActive` chưa kịp cập nhật;
  nhờ đó nút Chọn đúng tab luôn nhận trạng thái active ngay khi chuyển.
- Panel thuộc tính Edit shape/drawing comment do `DocumentWorkspace` quản lý theo selection
  hiện tại.
- Chỉ khi View còn active, chuyển
  `PDFSearchController::resultActivated(page, x, y, resultsRevision)` thành
  `searchResultRequested(...)` để viewport cuộn tới đúng match chuẩn hóa;
  `Main.qml` giữ request kèm revision nếu workspace bất đồng bộ chưa tạo xong.
- Khi chọn Forms tool, bảo đảm Text Field, Check Box, Radio Button, Dropdown,
  List Box và Comb Field loại trừ nhau.
- Chuyển tiếp signal Open/Save/Save As, page, delete page, zoom, fit, rotate và
  tool placeholder lên cửa sổ.

**Ràng buộc:** ribbon chỉ phát ý định và cập nhật tool state; không trực tiếp mở
dialog, đổi tab hoặc ghi tệp.

### [`src/Components/ViewRibbon.qml`](../src/Components/ViewRibbon.qml)

**Vai trò:** nhóm lệnh đọc và điều hướng tài liệu.

- Nhận `document`, `editController`, `searchController` và `session` để
  enable/disable lệnh theo `loaded`/`saving`, giữ tìm kiếm và Page Display đúng
  tab.
- Phát Open, Save, Save As; trang trước/sau hoặc số trang yêu cầu; Zoom Out/In,
  Fit Width và Rotate Left/Right.
- Gắn `PageDisplayControl` dạng dropdown gọn trong nhóm Page Display, bind trực
  tiếp mode/cover/split của `PDFDocumentSession` và chuyển intent cho Single
  Page, Continuous, Facing, Continuous Facing, Separate Cover Page cùng hai kiểu
  Split lên `AppRibbon`.
- Gắn `SearchRibbonControl` trong nhóm Search để tìm kiếm tài liệu ngay
  trên ribbon.
- Dùng `RibbonGroup` và `RibbonButton`, nên logic enable/split button phải thống
  nhất với component dùng chung.

### [`src/Components/PageDisplayControl.qml`](../src/Components/PageDisplayControl.qml)

**Vai trò:** control Page Display dạng dropdown trong tab View.

- Bốn nút layout chọn `SinglePageMode`, `ContinuousMode`, `FacingMode` hoặc
  `ContinuousFacingMode`; trạng thái active đọc trực tiếp từ session.
- `Separate Cover Page` là toggle, chỉ enable khi mode hiện tại là Facing hoặc
  Continuous Facing. Nó không tạo thêm một layout độc lập.
- Hàng Split là action ghép trong dropdown: phần chính bật/tắt Vertical Split,
  còn mũi tên mở menu con chọn Vertical hoặc Horizontal Split.
  Icon phản ánh kiểu đang dùng.
- Phát signal có kiểu cho đổi mode, cover và `SplitViewMode`. Component không tự
  đổi layout hay giữ bản sao state; `Main.qml`/`DocumentWorkspace` áp intent lên
  session đang active.

### [`src/Components/SearchRibbonControl.qml`](../src/Components/SearchRibbonControl.qml)

**Vai trò:** command surface tìm text trong tài liệu của tab View.

- Ô nhập giới hạn 256 ký tự và chuyển text vào controller của session ngay;
  controller sở hữu debounce 300 ms nên input không mất khi component bị hủy.
  IME composition chỉ chuẩn bị query cho tới commit; Enter tìm ngay, Escape hoặc
  nút Clear xóa query, kết quả và highlight.
- Hiển thị Searching, Failed, No results hoặc vị trí `%1 / %2`; trạng thái
  `Limited` thêm dấu `+` để không ngụ ý đã giữ toàn bộ kết quả.
- Previous/Next gọi navigation vòng của `PDFSearchController`; component không
  tự scroll trang hoặc giữ bản sao danh sách kết quả.
- `Connections` đồng bộ lại text khi controller/tab đổi hoặc bị clear do đổi
  document/cấu trúc trang. Mọi label, tooltip và accessible name đều dùng
  translation ID.

### [`src/Components/ReaderControlBar.qml`](../src/Components/ReaderControlBar.qml)

**Vai trò:** thanh điều khiển đọc cố định ở đáy cửa sổ.

- Hiển thị First/Previous, ô nhập số trang một-based và tổng trang,
  Next/Last; các nút biên tự disable theo trang hiện tại.
- Giữ tối đa 64 trạng thái trang/zoom cho Previous View và Next View, tách theo
  identity của session và pane; một drag slider chỉ tạo một bước history.
- Quan sát page/zoom đã bind nên history nhận cả thay đổi từ ribbon, tìm kiếm và
  cuộn trang. Việc ghi chỉ bắt đầu sau initial fit, còn đổi cấu trúc trang sẽ
  khởi tạo lại history của context để không phục hồi page index cũ.
- Cung cấp bốn nút nhanh cho Single, Continuous, Facing và Continuous Facing,
  bind trực tiếp `pageDisplayMode` của session đang active.
- Zoom dùng thang logarit trong khoảng 10–500%, có nút bước, slider và danh sách
  preset; mọi đường điều khiển đều đi qua clamp và API zoom của workspace.
- Chỉ phát intent. `Main.qml` và `DocumentWorkspace` quyết định pane nhận page/
  zoom, nên footer không giữ document hoặc controller và không can thiệp pooling.

### [`src/Components/HelpToolTip.qml`](../src/Components/HelpToolTip.qml)

**Vai trò:** tooltip hướng dẫn dùng chung, với tiêu đề tùy chọn và nội dung tự xuống dòng.

- Nhận `theme`, `title` và `text`; nội dung được dịch tại nơi sử dụng.
- Gán `parent` là control cần hướng dẫn và bind `visible` theo hover/focus.
  Dùng `Loader` khi tooltip chỉ cần trong menu hoặc popup đang mở.
- Mặc định hiện bên phải control, rộng 340 px, trễ 500 ms và không tự hết hạn.
  Có thể ghi đè `x`, `y`, `width`, `delay`, `timeout` của `ToolTip` khi tái sử dụng.
- Không phụ thuộc controller hay chức năng PDF. `EditRibbon` dùng component này
  cho hướng dẫn Thêm hoặc sửa liên kết.

### [`src/Components/EditRibbon.qml`](../src/Components/EditRibbon.qml)

**Vai trò:** toàn bộ command surface cho edit nội dung và page operation.

**Tool và command:**

- Select, Add Text, Add Image.
- Link mở menu gồm Thêm hoặc sửa liên kết, Tự động tạo liên kết từ URL và
  Xóa liên kết web. Mục tự động gọi `autoCreateWebLinks()` và đổi thành Hủy tạo
  liên kết khi đang quét. Xóa liên kết web gọi `removeWebLinks()` và đổi thành
  Hủy xóa liên kết trong lúc quét; các lệnh quét loại trừ nhau. Thêm hoặc sửa
  liên kết gọi `beginLinkEditing()` để bật `LinkTool` và tải các vùng link nguồn.
- `ManualLinkLayer.qml` nằm trên `EditOverlay`: kéo vùng trống để tạo, chọn/nhấp
  đúp link để sửa, di chuyển và resize bằng 8 handle, Delete và menu chuột phải.
  `LinkPropertiesDialog.qml` được tải lười, chỉ tạo draft khi người dùng áp dụng;
  chọn URL hoặc trang trong PDF cùng viền, màu, độ dày, kiểu nét, highlight.
  Link nguồn chưa sửa đi qua native Browse; link draft nhận click qua controller.
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

### `src/Components/WebLinkDraftLayer.qml`

Hiển thị vùng liên kết tự động chưa lưu trong cùng `pageSurface`: Edit có khung
cam, Browse nhận click/bàn phím và hiện URL khi hover. Layer không mở URL trực
tiếp mà gọi `activateWebLink(page, index, revision)` để xác thực lại draft và đi
qua xác nhận HTTP/HTTPS. Dữ liệu đọc từ `webLinkDraftsForPage()` theo revision;
delegate pooled không giữ link region. Mỗi trang tối đa 512 rectangle; Link
không thuộc chỉ mục Comment hoặc candidate text. Sau Save, annotation đã lưu do
PDFium xử lý. Link giữ vị trí độc lập khi text bên dưới được chỉnh sửa.

### [`src/Components/CommentRibbon.qml`](../src/Components/CommentRibbon.qml)

**Vai trò:** command surface cho PDF comment/markup, nằm ngay sau tab Edit.

- Select nằm đầu ribbon, cùng vị trí với Select của tab Edit. Khi tab mở,
  `AppRibbon` bật `PDFEditController.CommentSelectTool` và nút hiển thị trạng
  thái `primary`; mode này chỉ chọn comment candidate. Người dùng có thể chuyển
  sang Sticky Note, Free Text, text markup, Pencil hoặc năm drawing-comment
  tool mà không bật form design mode.
- Năm drawing-comment tool được gom trong một dropdown Shapes ba cột, cùng kiểu
  trình bày và tương tác với dropdown Shapes của tab Edit.
- Highlight, Underline, Squiggly và Strikeout là split button: phần chính dùng
  màu hiện tại, menu có bảng màu curated và system color picker cùng kiểu.
  Underline, Squiggly và Strikeout dùng palette RGB đậm và opacity cao hơn để
  các nét mảnh vẫn rõ. Mỗi tool giữ màu riêng qua
  `editController.highlightColor`, `editController.underlineColor`,
  `editController.squigglyColor` hoặc `editController.strikeoutColor`.
- Cả bốn text-markup tool dùng chung gesture kéo vùng, draft history và pipeline
  lưu annotation. Rectangle kéo chỉ là đầu vào: controller hit-test glyph
  source-text đã cache, bỏ whitespace và snap kết quả theo từng dòng trước khi
  tạo draft. Gesture gần như nằm ngang được mở thành dải hit-test
  tối thiểu 3 px, nên không bị bỏ chỉ vì độ lệch dọc của pointer quá nhỏ. Khi
  cache glyph còn tải, controller giữ gesture cùng tool/màu và tự hoàn tất sau
  scan thay vì yêu cầu người dùng kéo lại. OCR ẩn tham gia hit-test nhưng không xuất hiện như
  text candidate để chọn/sửa. Kéo vùng không có glyph khả dụng không tạo
  annotation; hiện glyph phải ngang/upright, còn page không có text layer khả
  dụng hoặc vượt safety cap 50.000 ký tự không hỗ trợ bốn text-markup tool.
  Sticky Note đặt bằng
  một lần bấm. Free Text nhận click hoặc drag để tạo hộp chữ; cả hai chỉnh nội
  dung trong panel bên phải. Chọn Free Text cũng mở panel nếu panel đang đóng.
- Rectangle/Oval/Line dùng drag; Polygon/Polyline dùng multi-click, Enter hoặc
  nhấp đúp để hoàn tất. Cả năm dùng màu drawing riêng và renderer vector chung.
- Pencil dùng màu drawing chung và một drag liên tục để tạo đúng một Ink
  annotation. Gesture/draft đều dùng `InkDrawingView`; model vẫn nhận nhiều
  stroke khi rehydrate `/InkList` nguồn.
- Stamp là split button với 24 preset từ `controller.stampPresets`, chia thành
  Dynamic (5), Standard (12), Sign Here (5) và Additional (2). Nhóm Additional
  giữ For Comment và Not Approved. Menu dùng `StampView` để preview artwork và
  màu; phần chính giữ preset hiện tại rồi chuyển sang `StampTool`. Dynamic chụp
  tác giả/ngày giờ lúc đặt; các lần sửa nội dung không đổi dòng thông tin này.
- Cả sáu drawing subtype Square, Circle, Line, Polygon, PolyLine và Ink đã tạo
  hoặc source hợp lệ dùng cùng selection frame như Foxit: kéo toàn bounding box
  hoặc dùng phím mũi tên để di chuyển, kéo tám handle cạnh/góc để đổi kích thước.
  Frame scale endpoint, vertex hoặc toàn bộ stroke tương ứng; Escape/cancel
  session trả frame và geometry về trạng thái ban đầu. Source chỉ nhận geometry
  interaction khi annotation permission và các cờ
  ReadOnly/LockedContents/Locked cho phép; capability sửa style vẫn được gate
  riêng theo độ an toàn round-trip của `/AP`. Stamp nguồn hợp lệ cũng dùng
  bounding-box drag/phím mũi tên và tám handle; Save chỉ đổi `/Rect`, giữ `/AP`
  và appearance `/BBox`.
- Gesture drawing có owner page duy nhất ở `PDFEditController`; bắt đầu trên
  trang khác sẽ hủy polygon/polyline hoặc Ink tạm của delegate trước để pooling
  không giữ hai path dang dở song song.
- Nhóm Manage bật/tắt danh sách comment; nhóm History dùng chung undo/redo của
  `PDFEditController`.
- Undo/Redo dùng chung history của `PDFEditController` và điều hướng tới trang
  bị thay đổi qua `pageRequested()`.

### [`src/Components/CommentPanel.qml`](../src/Components/CommentPanel.qml)

**Vai trò:** panel bên phải cho danh sách comment toàn tài liệu, nội dung Sticky
Note/Free Text đang chọn và phản hồi một cấp.

- Khi được load, panel yêu cầu controller chạy annotation-only index tuần tự ở
  nền cho mọi source page; kết quả không phụ thuộc cache candidate 12 trang của
  viewport và tự hủy khi document/revision đổi.
- Hiển thị loại, trang, màu, nội dung rút gọn và trạng thái draft; chọn item sẽ
  chuyển về Select, chọn candidate tương ứng và điều hướng tới trang đó.
- Comment được hỗ trợ, gồm Sticky Note, Free Text, text markup, Stamp, cả sáu
  drawing subtype và reply có sẵn trong PDF, dùng `TextArea` để sửa `/Contents`;
  nội dung được buffer theo `page + objectId` và commit khi mất focus, thu gọn
  panel, đổi session hoặc thực hiện Save.
- Hai editor có bộ đếm trực tiếp và soft-limit 700 UTF-16 code unit. Nội dung có
  thể tạm vượt giới hạn để không phá IME/undo, nhưng Gửi, commit và Save bị chặn
  cho tới khi người dùng rút gọn về 700 hoặc ít hơn.
- Comment gốc có action Phản hồi và composer Gửi/Hủy; Ctrl/Cmd+Enter gửi, Esc
  hủy. Nút dropdown trên từng comment gốc bung/thu nhóm reply; reply được thụt
  vào kèm đường nhánh cấp hai trong cùng model phẳng và không có action phản
  hồi tiếp, nên UI luôn chỉ có một cấp con. Reply chỉ hiện trong panel; API dữ
  liệu theo trang và selection decoration của `EditOverlay` không vẽ reply lên
  viewer. Annotation chuyển trạng thái do Foxit/reader khác tạo với `/State` và
  `/StateModel /Review` hoặc `/Marked` bị loại khỏi model, reply count và
  selection; controller vẫn giữ chúng nội bộ để kiểm permission và cascade
  thread an toàn.
- Hàng action của comment gốc và reply có nút ba chấm ở mép phải; nút Phản hồi
  vẫn chỉ có trên comment gốc. Menu dùng chung được tạo khi cần bằng `Loader`,
  gồm `Set Status` với submenu cascade khi hover
  (None, Accepted, Rejected, Cancelled, Completed, Deferred, Future) và
  `Mark with Checkmark`. Các action bind checked/enabled theo model, gọi
  `setCommentStatus()` hoặc `setCommentMarked()` và tạo draft có
  undo/redo/discard; status hiện hành và checkmark được hiển thị bằng badge trên
  đúng card comment gốc hoặc reply. Save ghi annotation state chuẩn, giữ history
  độc lập theo annotation đích rồi reopen cập nhật model.
- Comment có sẵn trong PDF được liệt kê để đọc, điều hướng, sửa nội dung và xóa
  dưới dạng draft có undo/redo/discard. Annotation ReadOnly/LockedContents vẫn
  được bảo vệ. Xóa comment gốc ẩn cả thread, còn xóa reply chỉ tác động item đó.
  Panel dùng chung trạng thái thu gọn theo document session.
- Square, Circle, Line, Polygon, PolyLine và Ink có kind riêng; cả draft mới lẫn
  source hợp lệ đều hỗ trợ chọn, drag/phím mũi tên, resize tám handle, sửa nội
  dung, xóa, reply và history.
  Panel này chỉ xử lý nội dung/thread; stroke, fill và opacity được tách sang
  `DrawingCommentPropertiesPanel` trong tab Edit. Stamp nguồn dùng selection
  frame để move/resize nhưng không dùng drawing-style panel; Caret,
  FileAttachment, Sound, Redact và Projection dùng card `comment` chỉ đọc. Ink có `/InkList` malformed
  hoặc vượt giới hạn scan 64 stroke, 4.096 điểm/stroke hay 32.768 điểm tổng cũng
  hạ xuống card generic chỉ đọc thay vì cấp phát dữ liệu không giới hạn.

### [`src/Components/ImagePropertiesPanel.qml`](../src/Components/ImagePropertiesPanel.qml)

**Vai trò:** panel **Thuộc tính ảnh** ở cạnh phải, chỉ hiện cho ảnh đang chọn
trong tab Edit.

- `DocumentWorkspace` load panel khi tài liệu đã mở và selection có kind `image`
  hoặc `addedImage`. Panel tự mở rộng khi vừa được kích hoạt, thay
  `TextFormatPanel`, và ẩn khi bỏ chọn ảnh hoặc rời Edit.
- Checkbox **Khóa tỷ lệ ảnh** bind `imageAspectRatioLocked` trên controller riêng
  của tab, mặc định bật. Toggle không tạo draft/history hay thuộc tính PDF;
  checkbox bị vô hiệu hóa khi `selectedImageResizable` là false hoặc đang lưu.
- Tám resize handle và geometry gesture vẫn thuộc `EditOverlay`; popup khi chọn
  ảnh chỉ giữ action Delete.

### [`src/Components/DrawingCommentPropertiesPanel.qml`](../src/Components/DrawingCommentPropertiesPanel.qml)

**Vai trò:** panel thuộc tính chung cho Edit shape và drawing comment đang chọn
trong tab Edit; không lập chỉ mục, hiển thị hay sửa nội dung comment/thread.

- `DocumentWorkspace` load panel khi selection là `addedShape`, `sourceShape`
  hoặc Square/Circle/Line/Polygon/PolyLine/Ink và tab Edit đang active; panel tự
  mở rộng khi vừa được kích hoạt và thay `TextFormatPanel` ở cùng vị trí cạnh
  phải. Edit shape dùng tiêu đề **Thuộc tính hình vẽ**, drawing comment giữ
  **Thuộc tính chú thích hình vẽ**.
- API `selectedDrawing*` dùng chung cho stroke, fill/no fill của hình đóng và
  opacity. `ColorDialog` được load theo
  nhu cầu; slider chỉ commit một command khi kết thúc pointer gesture và mọi
  callback giữ identity `page + objectId` để không áp vào selection mới.
- Edit shape mới và `sourceShape` đã validate có thể sửa thuộc tính và lưu lại;
  QML không tự nhận diện path nguồn hoặc đọc metadata. Added comment luôn sửa
  được. Source drawing comment chỉ bật control khi
  controller xác nhận appearance có thể round-trip; source ngoài/custom vẫn
  hiển thị giá trị nhưng khóa chỉnh sửa.
- Trạng thái thu gọn dùng chung `session.panelCollapsed` với các panel phải khác.

### [`src/Components/FormRibbon.qml`](../src/Components/FormRibbon.qml)

**Vai trò:** ribbon tối giản cho form designer hiện hỗ trợ đặt field.

- Nhận document và sáu cờ lựa chọn từ `AppRibbon`.
- Phát signal tool cho Text Field, Check Box, Radio Button, Dropdown, List Box
  hoặc Comb Field; Comb dùng icon riêng.
- Chỉ enable khi document đã load.
- Date/Signature vẫn là điểm mở rộng; ribbon chỉ quảng bá sáu loại đã có
  pipeline lưu hoàn chỉnh.

### [`src/Components/SignRibbon.qml`](../src/Components/SignRibbon.qml)

**Vai trò:** ribbon cho chữ ký hiển thị trên trang PDF.

- Nhận theme/document/edit controller, dùng biến thể màu xanh cho nhóm ký.
- Phát intent Chọn, Chữ ký, Ký tắt, Ngày và Dấu kiểm; nhóm history/file có
  Undo/Redo và Save/Save As theo trạng thái controller.
- `AppRibbon` chuyển intent tạo chữ ký/ngày/dấu tới `Main.qml`; ribbon không
  trực tiếp tạo ảnh, mở dialog hoặc lưu PDF.

### [`src/Components/SignatureDialog.qml`](../src/Components/SignatureDialog.qml)

**Vai trò:** chuẩn bị chữ ký/ký tắt hoặc ngày/dấu trước khi đặt lên trang.

- Bốn tab chữ ký: vẽ các nét rời, nhập tên với kiểu chữ/màu, chọn ảnh cục bộ với
  tùy chọn bỏ nền trắng, hoặc chọn mẫu đã lưu theo loại chữ ký/ký tắt.
- Giữ gesture có giới hạn và debounce preview; `SignatureLibrary` kiểm tra dữ
  liệu và tạo PNG nền trong suốt. Thay đổi đầu vào xóa preview cũ trong lúc chờ.
- Mẫu có thể được đặt tên, lưu hoặc xóa trong thư viện cục bộ. Ngày dùng ngày
  ISO đã kiểm tra với ba định dạng; dấu có lựa chọn check hoặc cross.
- Chỉ phát `placementRequested(widthPoints)` khi preview hợp lệ. Main chuyển
  sang cursor đặt ảnh; Cancel/Escape và unload dọn timer/file dialog.
- Tạo ảnh chữ ký hiển thị, không tạo field ký số hay quản lý chứng thư.

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

- Bind `session.document`, edit/form/designer/search controller vào
  `DocumentView`.
- Hai chiều hóa zoom và vị trí scroll với `session.zoom`, `scrollX`, `scrollY`;
  truyền `session.viewRotation`, `pageDisplayMode`, `separateCoverPage` và trạng
  thái tab/tool form.
- `NoSplit` tạo một viewport. Vertical/Horizontal Split tạo hai `DocumentView`
  theo trái-phải hoặc trên-dưới; mỗi pane giữ zoom, scroll và current page riêng,
  và toolbar điều hướng/zoom pane vừa được kích hoạt.
- Pane phụ tắt interaction Edit/Form; pane chính vẫn là chủ sở hữu selection và
  controller tương tác. Workspace lưu/khôi phục `PDFViewState` của pane phụ trước
  khi đổi split mode hoặc phá workspace.
- Thực hiện Fit Width từ kích thước viewport và maximum page dimension đã xoay;
  initial-fit chỉ chạy khi session còn `initialFitPending`.
- Load đúng một panel ở cạnh phải: `CommentPanel` khi Comment active và danh
  sách được bật; `DrawingCommentPropertiesPanel` khi tab Edit đang active và
  chọn `addedShape`/`sourceShape` hoặc Square/Circle/Line/Polygon/PolyLine/Ink;
  `ImagePropertiesPanel` khi tab Edit hoặc Sign active và chọn `image`/`addedImage`;
  `TextFormatPanel` cho các trạng
  thái Edit còn lại; hoặc panel Text Field, Comb Field, Dropdown/List Box và
  Radio Button tương ứng trong Forms. Panel thuộc tính ảnh/shape/comment có ưu
  tiên hơn `TextFormatPanel`. Khi một panel thuộc tính vừa được kích hoạt, nó tự
  mở rộng; trạng thái collapsed được lưu theo session.
- Chuyển các signal font family, bold/italic/underline, color và paragraph
  alignment vào `PDFEditController`.
- Header > Add Text bật trạng thái đặt chữ; click trong vùng Header tạo textbox
  inline và mở `HeaderTextPanel` qua Loader riêng. `HeaderTextPlacement` giữ text
  và vị trí khi delegate bị pool; input chưa Áp dụng không tham gia draft guard.

**Vòng đời:** `prepareForDeactivation()` yêu cầu `DocumentView` kết thúc editor,
hủy interaction và lưu scroll trước khi workspace bị phá; hàm cũng chạy trong
`Component.onDestruction` để không phụ thuộc caller luôn nhớ gọi.

### [`src/Components/HeaderTextPanel.qml`](../src/Components/HeaderTextPanel.qml)

**Vai trò:** chọn tất cả trang hoặc phạm vi `1-3, 5` sau khi đặt textbox trên trang.
Nút Áp dụng gọi `PDFEditController.addHeaderText(text, pages, xPoints, yPoints, widthPoints)`; thành công đóng
panel, thất bại giữ input và hiện lỗi. Hủy không tạo draft. Đổi tab/session,
nguồn PDF, cấu trúc trang, history hoặc bắt đầu Save sẽ hủy input tạm. Undo/Redo
trong ô nhập dùng history cục bộ của ô, không gọi history PDF. Chữ đã Áp dụng
dùng preview và Save của added text; xem [pipeline](header-text-pipeline.md).

### [`src/Components/HeaderTextPlacement.qml`](../src/Components/HeaderTextPlacement.qml)

**Vai trò:** state tạm của workspace: chờ click, trang/vị trí/chiều rộng theo pt,
text, lỗi và Apply/Cancel. Nội dung không phụ thuộc delegate đang hiển thị.

### [`src/Components/HeaderTextOverlay.qml`](../src/Components/HeaderTextOverlay.qml)

**Vai trò:** nhận click trong vùng xám khi Add Text được bật, hiển thị textbox
inline bằng Loader trên `pageSurface` (z=2) để cùng zoom/xoay với PDF. Click đổi
pixel sang pt đúng một lần. Panel không còn chứa ô nhập text.

### [`src/Components/PageThumbnailPanel.qml`](../src/Components/PageThumbnailPanel.qml)

Thanh hình thu nhỏ bên trái workspace, thu gọn theo `session.thumbnailsVisible`.
`ListView` tái sử dụng delegate với cache một hàng; mỗi `PDFPageView` nằm trong
khung tối đa 144 × 168 logical pixel, giữ DPR gốc và budget raster chung.
Delegate pooled tách document/form controller; thu gọn hủy Loader/list/texture.
Viền cam và tự cuộn theo trang của pane đang active. Click đi qua guard commit
panel/editor/form của workspace rồi điều hướng pane đó. Dùng nội dung trang đã
lưu (không gắn Edit controller để tránh suppression thiếu overlay); bản nháp
nội dung cập nhật sau Save, có nhắc lưu. Cấu trúc trang, xoay view và giá trị form
runtime cập nhật ngay. Thumbnail không có editor, search highlight hay input form.

### [`src/Components/DocumentView.qml`](../src/Components/DocumentView.qml)

**Vai trò:** viewport ảo hóa cho bốn bố cục trang và nơi quản lý navigation.

**Cấu trúc và state:**

- `ListView` dọc có `reuseItems: true`, `clip: true` và cache buffer bảo thủ.
  Single Page và Facing đặt model đúng một delegate, rồi ánh xạ delegate đó tới
  page/spread active; vì vậy không tồn tại hàng trước/sau để lộ một phần trong
  viewport. Trang hoặc spread được căn giữa mà không thay đổi zoom hiện tại.
- `contentWidth` được khai báo theo chiều rộng trang lớn nhất sau xoay, zoom,
  số cột và khoảng cách của `PageLayoutDelegate`; không lấy `childrenRect` từ
  các delegate ảo hóa. Thanh cuộn ngang luôn hiện khi nội dung tràn viewport,
  ẩn khi nội dung vừa khung và giữ vị trí độc lập cho từng pane Split.
- Trong hai mode phân trang, wheel/phím trước hết thay đổi `contentX/contentY`
  bên trong page/spread đang phóng to. Chỉ khi chạm biên mới chuyển trang hoặc
  chuyển cả spread; trang kế tiếp được đặt ở đầu và trang trước ở cuối.
- Continuous dùng một trang mỗi hàng; Continuous Facing dùng một spread mỗi
  hàng. Cả hai giữ model đầy đủ nhưng chỉ materialize vùng đang thấy/gần thấy,
  dùng `pageGap` cấu hình được và cho cuộn xuyên suốt tài liệu.
- Delegate là `PageLayoutDelegate`. Ở mode continuous, current page là child có
  diện tích giao với viewport lớn nhất, không chỉ là child gần tâm; mỗi pane
  split giữ current page độc lập.
- Navigation ánh xạ display page sang hàng/spread rồi căn giữa đúng child page;
  search navigation vẫn map normalized point qua surface xoay của trang đích.
- Navigation theo lệnh dùng `pendingNavigationPage` và generation để tránh một
  callback scroll cũ ghi đè thao tác mới.
- Navigation kết quả map điểm normalized qua `pageSurface` đã xoay rồi căn giữa
  điểm đó trong viewport; một timer có generation/revision và deadline chờ
  delegate ảo hóa cùng page metrics sẵn sàng. Vì vậy match giữa/cuối trang không
  bị khuất ở zoom lớn và query cũ không replay sau khi bị clear/thay.
- Khôi phục `contentX/contentY`, expose viewport geometry cho workspace và fit.
- Khi vào Forms/Edit có source form fields, gọi chuẩn bị design mode một lần theo
  generation; theo dõi yêu cầu rollback page/focus từ form controller.
- Empty state cung cấp nút Open; toast trình bày `transientStatus`.

**API nội bộ quan trọng:** chuẩn bị session change, bắt đầu edit selected text,
đi tới trang, lưu/khôi phục vị trí và đồng bộ form design mode.

**Ràng buộc:** không tăng `cacheBuffer` tùy tiện; mỗi hàng có thể giữ hai texture
Retina đầy trang. Mọi state delegate phụ thuộc trang phải reset khi reused.

### [`src/Components/PageLayoutDelegate.qml`](../src/Components/PageLayoutDelegate.qml)

**Vai trò:** một hàng ảo hóa chứa một trang hoặc một spread hai trang.

- Ánh xạ `layoutIndex` thành `firstPage`/`secondPage`, kể cả quy tắc trang bìa
  đứng riêng, rồi đặt một hoặc hai `PdfPageDelegate` cạnh nhau. Trong mode phân
  trang, `layoutIndex` là spread/page active dù model chỉ có một delegate.
- Tính chiều rộng cột theo viewport, zoom, rotation và page lớn nhất; trang bìa
  hoặc trang lẻ cuối được căn giữa.
- Chuyển tiếp helper navigation/search từ `DocumentView` tới đúng child page và
  tính diện tích visible của từng child để đồng bộ `currentPage` trong spread.
- Sở hữu `ListView.onPooled/onReused`; khi pool sẽ tắt Loader trang thứ hai và
  đưa cả hai page delegate vào trạng thái cleanup.

### [`src/Components/PdfPageDelegate.qml`](../src/Components/PdfPageDelegate.qml)

**Vai trò:** một trang vật lý trong hàng layout, ghép nội dung PDF với mọi lớp
tương tác.

**Bố cục:**

- Tính kích thước trang từ `document.pageSize(page)`, zoom và quarter-turn.
- `pageSurface` dùng chung chứa `PDFPageView`, `SearchHighlightLayer` và
  `PageInteractionOverlay`; coordinator sau cùng chứa runtime form, edit và form
  designer.
- Rotation 90° được áp lên chính container dùng chung này; do đó texture, hit
  target form và overlay edit luôn xoay cùng nhau, còn tọa độ lưu không đổi.
- `PDFPageView` nhận edit/form/designer controller để render texture đã phản ánh
  suppression và draft preview.
- Delegate chuyển controller, mode, texture revision và viewport transform vào
  `PageInteractionOverlay`; coordinator xử lý z-order và cleanup chung.
- `SearchHighlightLayer` nằm ở `z: 0.5`, dưới coordinator tương tác; nó đọc rect
  normalized của đúng display page và tự xoay cùng `pageSurface`.
- `headerAreaOverlay` chỉ hiện khi Header active và tài liệu đã mở, cao
  `100 * zoom` (giới hạn trong chiều cao trang), phủ toàn chiều rộng ở đầu
  `pageSurface`. Dải xám bán trong suốt ở `z: 0.75` xoay cùng trang, không nhận
  input, không tạo draft và không lưu vào PDF; áp dụng cho cả hai pane Split.
- Hiển thị số trang ngoài page surface.

Xem [overlay-architecture.md](overlay-architecture.md) để có cây item, z-order
và ma trận mode/input đầy đủ của các nhánh này.

**Pooling:** nhận cờ `pooled` từ `PageLayoutDelegate`; khi true, child ngắt
document, interaction và focus. Wrapper bật lại khi hàng được reuse để request
state của page mới.

### [`src/Components/SearchHighlightLayer.qml`](../src/Components/SearchHighlightLayer.qml)

**Vai trò:** vẽ highlight tìm kiếm không tương tác trên một trang đang hiện.

- Đọc `highlightsForPage(page)` lại khi `resultsRevision` thay đổi; mỗi rect được
  đổi từ normalized geometry sang pixel bằng kích thước page surface. Controller
  giới hạn 512 rect/page và luôn đưa current match vào tập hiển thị.
- Kết quả thường dùng vàng bán trong suốt; mọi rect thuộc match hiện tại dùng
  cam và viền cam.
- `enabled: false`, không bắt pointer/keyboard và nằm dưới toàn bộ
  `PageInteractionOverlay`. Khi delegate pooled, layer ẩn và không giữ geometry
  mutable riêng của page cũ.

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

**Dữ liệu vào:** page, page-point scale, trạng thái pooled, hai cờ texture draft,
snapshot object ID của source comment đang bị texture ẩn, theme và
`PDFEditController`. Component tải
`candidatesForPage()`/`draftsForPage()` và làm mới khi controller báo page
data/candidate/history/selection đổi.

**Hit testing và con trỏ:**

- Chuyển pointer pixel sang tỉ lệ trang và gọi candidate cache của controller.
- `SelectTool` và `CommentSelectTool` dùng chung interaction chọn, nhưng
  `candidateMatchesSelectionTool()` lọc hit-test, draft delegate và keyboard
  traversal của mode Comment xuống đúng các kind comment trước khi chọn.
- Với cả bốn text-markup tool, QML chỉ gửi rectangle chuẩn hóa cho
  preview/commit và render geometry đã được controller snap theo glyph/dòng.
  Geometry glyph đến từ page scan bất đồng bộ/cache; QML không truy cập handle
  PDFium và không tự dò text.
- Có hit-test riêng cho rectangle/oval/line/arrow/polygon với tolerance theo
  kích thước trang.
- Quản lý con trỏ Select/Add Text bằng SVG và fallback cursor; ẩn cursor khi
  pooled, ngoài trang hoặc tool không phù hợp.
- Các công cụ draw shape/text-markup/Free Text/Stamp/drawing comment giữ point tạm thời,
  validate gesture rồi mới tạo một draft command. Click Free Text dùng kích
  thước mặc định; Stamp click dùng 160 × 50 PDF point; drag của cả hai dùng
  rectangle đã clamp trong trang.
- Pencil chỉ append mẫu khi cách mẫu trước ít nhất 1 logical pixel, giữ tối đa
  4.096 raw points và tiếp tục cập nhật endpoint cuối khi đã đầy. Release gửi
  đúng một stroke normalized sang controller;
  pooling, đổi tool/session và Escape đều hủy gesture tạm.

**Text rich-edit:**

- Chuyển character spans và paragraph formats thành rich HTML cho `TextEdit`;
  escape text/attribute/color và hiệu chỉnh font stretch hiển thị.
- Phân biệt source bounds bất biến, layout frame và laid-out content bounds.
- Khi mở source text, frame có thể mở rộng transient tới biên cột; chỉ controller
  quyết định khi nào geometry trở thành draft thật.
- Đồng bộ selection range, typing format, cỡ chữ và B/I/U với `QTextDocument` mà không
  thay toàn bộ document, nhờ đó local undo/redo của editor không bị mất.
- Commit/cancel editor, sync content bounds sau layout và chuyển focus an toàn
  khi session/tool/selection đổi.

**Draft presentation:**

- Mask source rect khỏi texture sạch khi cần, rồi vẽ
  text/image/shape/text-markup/Free Text draft lên trên. Free Text dùng hộp nền
  trắng, viền đen và text wrap khớp appearance lưu. FreeText nguồn không đổi giữ
  appearance gốc; khi sửa nội dung, draft được preview bằng style hộp mặc định
  này. Nếu render worker đã trả texture draft đúng revision, component tránh
  chồng preview không cần thiết.
- `DraftImageView` vẽ pixel snapshot của ảnh thêm mới; `ShapeDraftView` vẽ vector
  cho `addedShape` và draft `sourceShape`. Shape nguồn chưa có draft giữ hình
  trong PDF texture; khi undo/discard, `originalPreview` phục hồi hình gốc trong
  lúc đợi texture. Hit-test shape nguồn đã chọn dùng candidate nếu chưa có draft.
- `CommentDrawingView` vẽ Square/Circle/Line/Polygon/PolyLine từ cùng geometry
  và common properties mà writer dùng để dựng `/AP`. Khi geometry source của
  năm subtype này đổi, delegate vẽ endpoint/vertex đã translate/scale trên
  texture đã suppress appearance nguồn.
- `InkDrawingView` vẽ preview Pencil và từng stroke draft bằng Qt Quick Shapes
  với round cap/join, cùng màu, opacity và border width của save snapshot. Khi
  geometry Ink nguồn đổi, delegate vẽ stroke đã translate/scale trên texture đã
  suppress appearance nguồn.
- Style-only draft cũng suppress appearance drawing nguồn và dùng delegate hiện
  hành để preview stroke/fill/opacity, nhưng chỉ khi object ID của nó có trong
  suppression snapshot của texture đang được trình bày. Khi undo/discard,
  overlay giữ restoration preview đúng cho từng object cho tới khi texture sạch
  thay thế snapshot đó; content-only draft không bị vẽ chồng source appearance.
- `StampView` vẽ nền màu và artwork vector từ `PDFStampPresets.h` dùng chung
  cho menu preset, preview placement và draft Stamp mới; artwork Dynamic đã đặt
  giữ tác giả/ngày giờ từ snapshot của candidate.
- Hiển thị bounding box, hover box, contextual Edit/Delete và resize handles.
- Image mới và image nguồn đủ điều kiện có tám handle cạnh/góc. Popup khi chọn
  ảnh giữ Delete; checkbox **Khóa tỷ lệ ảnh** nằm trong `ImagePropertiesPanel`
  do workspace quản lý. Resize đọc `imageAspectRatioLocked` trên controller
  riêng của tab để giữ tỷ lệ frame tại đầu gesture khi khóa bật, hoặc cho đổi
  chiều rộng/cao độc lập khi tắt. QML dùng `selectedImageResizable` từ scanner
  C++; source image có custom clip, matrix không hợp lệ hoặc vượt biên page
  không có resize handle, nhưng vẫn giữ move/delete hiện có. Image lồng trong
  Form XObject tiếp tục được bảo vệ.
- Drag text/image/shape, cả sáu drawing subtype và Stamp mới/nguồn giữ preview
  trong QML nhưng chỉ commit geometry một lần khi thả; phím mũi tên tạo move command,
  Shift dùng bước lớn hơn. Sáu Edit shape mới/nguồn hợp lệ, cả sáu drawing subtype
  mới hoặc source hợp lệ và
  Stamp mới/nguồn hợp lệ nhận tám resize handle ở cạnh/góc; frame scale
  endpoint/vertex của Square/Circle/Line/Polygon/PolyLine hoặc mọi stroke của
  Ink. Stamp nguồn chỉ đổi frame `/Rect`; `/AP` và `/BBox` được giữ nguyên.

**Dọn state:** `prepareForSessionChange()` kết thúc hoặc hủy editor/resize/draw,
ẩn cursor và xóa dữ liệu trang. Bất kỳ bổ sung gesture nào cũng phải đi qua
đường cleanup này và trạng thái `pooled`.

### [`src/Components/SquigglyLine.qml`](../src/Components/SquigglyLine.qml)

**Vai trò:** renderer dùng chung cho preview gesture và draft Squiggly.

- Nhận màu và `pagePointScale`; nét 1 PDF point, bước zigzag 2 PDF point được
  đổi sang logical pixel theo zoom của page delegate.
- Dùng cùng hình học đoạn thẳng mà PDFium sinh cho annotation Squiggly không có
  appearance stream, nên preview trước Save và texture sau Save giữ cùng độ dày,
  biên độ và tần số.
- Chỉ vẽ presentation; hit-test, snap glyph, QuadPoints và persistence vẫn thuộc
  `EditOverlay`, `PDFEditController` và `PDFSaveWriter`.

### [`src/Components/CommentDrawingView.qml`](../src/Components/CommentDrawingView.qml)

**Vai trò:** renderer vector riêng cho drawing comment, dùng chung giữa preview
gesture và added draft. Component nhận kind, vertices normalized, stroke/fill,
opacity, border width/style và dash pattern; không hit-test hoặc ghi PDF.

### [`src/Components/InkDrawingView.qml`](../src/Components/InkDrawingView.qml)

**Vai trò:** renderer freehand riêng cho Ink, dùng chung giữa path Pencil đang
kéo và draft đã tạo/rehydrate.

- Nhận mảng stroke normalized, màu, opacity và stroke width; mỗi stroke được vẽ
  thành `PathPolyline` độc lập bằng `Shape.CurveRenderer`.
- Dùng round cap/join và độ rộng tối thiểu 0,75 logical pixel để preview rõ ở
  mọi zoom. Component không validate/simplify hay ghi `/InkList`; controller và
  writer sở hữu các bước đó.

### [`src/Components/StampView.qml`](../src/Components/StampView.qml)

**Vai trò:** renderer nhẹ dùng chung cho preset Stamp trong ribbon, preview khi
click/drag và draft chưa Save.

- Nhận `stampName`, `stampText`, `stampColor` và `stampArtwork`. Khi có artwork,
  vẽ nền màu và path trắng bằng Qt Quick Shapes, scale từ frame chuẩn 160 × 50;
  path gồm viền, icon và chữ Unicode đã outline, kể cả dòng tác giả/ngày giờ.
- Chỉ vẽ presentation. `PDFStampPresets.h` tạo path chung cho QML và normal
  `/AP`; controller sở hữu catalogue, snapshot Dynamic, geometry và history.
  Stamp nguồn tiếp tục render AP gốc, giữ `/AP` và `/BBox` khi đổi frame.

### [`src/Components/ShapeDraftView.qml`](../src/Components/ShapeDraftView.qml)

**Vai trò:** renderer vector dùng lại cho preview trong menu, gesture đang vẽ và
draft shape mới hoặc `sourceShape` đang sửa.

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
- Cỡ chữ nhận số nguyên 1–512 PDF point, đồng bộ từ selection/caret và để trống
  khi vùng chọn có nhiều cỡ chữ. Dropdown, Enter hoặc mất focus gửi intent vào
  controller; đổi selection/session hủy input cũ. Ngoài editor áp toàn đối tượng,
  trong editor áp vùng chọn hoặc typing format tại caret; có undo/redo và Save.
- `collapsedChangeRequested` cho phép session giữ trạng thái thu gọn độc lập.

**Ràng buộc:** style button phải phản ánh state controller sau local editor
undo/redo, không chỉ state click gần nhất trong panel.

### [`src/Components/TextFieldOptionsPanel.qml`](../src/Components/TextFieldOptionsPanel.qml)

**Vai trò:** panel bên phải cho Text Field đang chọn trong Forms.

- Bật/tắt flag multi-line mà không làm mất Required/ReadOnly hiện có.
- Cho bật giới hạn ký tự tùy chọn; `0` nghĩa là không giới hạn, còn SpinBox nhận
  giá trị từ 1 đến 300.000.
- Mọi thay đổi gọi trực tiếp `PDFFormDesignerController`; khi hạ giới hạn dưới
  độ dài hiện tại, controller cắt value ngay trong draft form.
- Nút xóa ở cuối panel gọi `removeField()` cho cả Text Field mới tạo và
  source-backed do TrePDF tạo. Xóa source-backed chỉ là draft cho tới Save;
  Discard sẽ rehydrate field.
- Nội dung panel nằm trong `Loader` và dùng trạng thái collapsed của session.

### [`src/Components/CombFieldOptionsPanel.qml`](../src/Components/CombFieldOptionsPanel.qml)

**Vai trò:** panel bên phải riêng cho Comb Field đang chọn trong Forms.

- Chỉ hiển thị giới hạn ký tự bắt buộc; SpinBox nhận giá trị từ 1 đến 300.000
  và giá trị này đồng thời là số ô của field.
- Gọi `PDFFormDesignerController::setFieldMaxLength()` khi kết thúc chỉnh sửa;
  workspace flush thay đổi pending trước Save/Save As hoặc khi đổi session.
- Khi hạ giới hạn dưới độ dài value hiện tại, controller cắt value ngay trong
  draft form và giữ flag Comb.
- Nút xóa gọi `removeField()`; thay đổi chỉ ghi vào PDF khi Save/Save As.
- Nội dung panel nằm trong `Loader` và dùng trạng thái collapsed của session.

### [`src/Components/DropdownOptionsPanel.qml`](../src/Components/DropdownOptionsPanel.qml)

**Vai trò:** panel tùy chọn dùng chung cho Dropdown và List Box đang chọn.

- `listBoxMode` chọn nhãn, loại field mong đợi và thông báo lỗi tương ứng; panel
  đọc option hiện tại từ `PDFFormDesignerController` theo `fieldId`.
- Cho thêm, đổi tên và xóa option; trim chuỗi, cấm trùng và luôn giữ ít nhất một
  option trước khi gọi `setFieldOptions()`.
- Đồng bộ lại model cục bộ khi controller thay đổi selection/draft; lỗi cập nhật
  không làm panel giả vờ đã commit.
- Nút xóa gọi `removeField()` cho field mới hoặc source-backed do TrePDF tạo;
  thay đổi vẫn là draft cho tới Save/Save As.
- Nội dung có trạng thái collapsed theo session và dùng toàn bộ label/accessible
  name riêng cho Dropdown hoặc List Box.

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

- Khi Text Field, Check Box, Radio Button, Dropdown, List Box hoặc Comb Field tool active,
  MouseArea nền nhận click/drag.
- Click tạo kích thước mặc định; drag tạo rect chuẩn hóa. Check Box và Radio
  Button ép hình vuông; Text Field, Dropdown, List Box và Comb Field dùng hình chữ nhật và
  tôn trọng kích thước tối thiểu.
- Sau khi tạo Text Field hoặc Comb Field, lưu `pendingFocusFieldId` để editor
  chỉ focus sau khi model và loader đã sẵn sàng.

**Hiển thị và tương tác:**

- `FormFieldLayer` C++ vẽ field bằng scene graph, cung cấp active field id/rect/
  value/type/flags/source-backed và xử lý click/toggle/drag move.
- `Loader` tạo `TextField` native cho field text active; đồng bộ value vào
  controller nhưng tránh vòng lặp trong lúc nạp editor. Field có flag multi-line
  dùng `TextArea`, wrap nội dung và vẫn đồng bộ qua cùng controller. Field có
  flag comb vẽ divider, giới hạn input theo số ô, dùng cursor delegate rỗng để
  không hiển thị caret qua mọi vòng đời focus và ánh xạ click/selection theo
  từng ký tự Unicode.
- `Loader` tạo `ComboBox` cho Dropdown active từ options trong model và cập nhật
  selected value transient qua designer controller; với source-backed field,
  lựa chọn được chuyển qua PDFium form session rồi refresh lại model.
- `Loader` tạo `ListView` multi-select cho List Box active. Click hoặc
  Space/Enter bật/tắt dòng hiện hành; Up/Down/Home/End chỉ điều hướng;
  source-backed field dùng cùng identity-safe worker path.
- Active frame có bốn edge resize handles. Resize preview ở QML và commit qua
  `setFieldRect()` khi kết thúc; cancel phục hồi model.
- Field có thể kéo trực tiếp sau ngưỡng drag. Field text được chọn còn có move
  handle ở góc và hỗ trợ phím mũi tên; C++ clamp top-left để giữ nguyên kích
  thước trong trang rồi gọi `moveField()` đúng một lần khi kết thúc.
- Check Box và Radio Button có thể activate/toggle; chọn một Radio sẽ tắt member
  khác cùng Group ID. Dropdown và List Box có thể chọn option. Text Field thường và Check
  Box transient có nút xóa nổi; Text Field thường, Dropdown, List Box, Radio Button và
  Comb Field transient hoặc source-backed do TrePDF tạo được xóa từ panel tùy
  chọn. Discard phục hồi, còn Save/Save As mới gỡ Widget source-backed.
  Source-backed Check Box và cấu trúc AcroForm ngoài phạm vi TrePDF vẫn được bảo
  vệ.

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

**Vai trò:** nút chọn tab ribbon dùng chung.

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
| `PDFSearchController` | `PDFSearchController.cpp` | View ribbon và search highlight |
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

## Header image placement

| File | Responsibility |
| --- | --- |
| [`HeaderImagePlacement.qml`](../src/Components/HeaderImagePlacement.qml) | Ảnh tạm, khung PDF pt, giới hạn vùng 100 pt, batch Apply và hủy theo vòng đời. |
| [`HeaderImageOverlay.qml`](../src/Components/HeaderImageOverlay.qml) | Click chọn vị trí, preview, kéo và tám nút đổi kích thước. |
| [`HeaderImagePanel.qml`](../src/Components/HeaderImagePanel.qml) | Khóa tỷ lệ, xoay, tất cả/phạm vi trang, Áp dụng/Hủy. |

Backend dùng `prepareHeaderImage()`/`addHeaderImage()` trong
[`PDFEditControllerHeaderText.cpp`](../src/PDFEditControllerHeaderText.cpp).
Regression tests: [`backend`](../tests/tst_PDFEditControllerHeaderImage.cpp),
[`UI`](../tests/tst_PDFEditControllerHeaderImageUi.cpp).
