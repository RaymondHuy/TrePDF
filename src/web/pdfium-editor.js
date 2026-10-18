/* Browser-only, bounded draft editing. Every change is built in a fresh PDFium
 * document before replacing the visible one. Source edits target verified
 * top-level occurrences in the original content order. The desktop PDF edit pipeline is deliberately independent. */
"use strict";

class TrePdfEditor {
    constructor(engine, bytes, document, pages) {
        this.engine = engine;
        this.source = new Uint8Array(bytes);
        this.pages = pages;
        this.objects = [];
        this.savedObjects = [];
        this.history = [[]];
        this.historyIndex = 0;
        this.nextId = 1;
        this.selectedId = 0;
        this.editReason = engine._FPDF_GetSecurityHandlerRevision(document) !== -1
            ? "Protected PDFs are read-only in the web preview."
            : engine._FPDF_GetSignatureCount(document) !== 0
                ? "Signed PDFs are read-only in the web preview." : "";
        this.canEdit = !this.editReason;
        this.original = new TrePdfSource(this);
        this.images = new Map();
        this.nextImageRevision = 1;
    }

    state() {
        return { objects: this.original.merged(this.objects), sourceNotice: this.original.notice(), dirty: JSON.stringify(this.objects) !== JSON.stringify(this.savedObjects),
            canUndo: this.historyIndex > 0, canRedo: this.historyIndex + 1 < this.history.length,
            selectedId: this.selectedId, canEdit: this.canEdit, editReason: this.editReason };
    }

    validate(value, id) {
        if (!value || !["text", "rectangle", "ellipse", "line", "arrow", "triangle", "diamond"].includes(value.type))
            throw new Error("This editing tool is not supported in the web preview.");
        if (!Number.isSafeInteger(value.page) || value.page < 0 || value.page >= this.pages.length)
            throw new Error("Select a page in the current PDF.");
        this.validateGeometry(value);
        if (typeof value.color !== "string" || !/^#[0-9a-f]{6}$/i.test(value.color))
            throw new Error("Choose a valid object color.");
        const object = { id, type: value.type, page: value.page, x: value.x, y: value.y,
            width: value.width, height: value.height, color: value.color.toLowerCase() };
        if (value.type === "text") {
            if (typeof value.text !== "string" || value.text.length > 1024)
                throw new Error("Text is limited to 1,024 characters per object.");
            object.text = value.text.replace(/\r\n?/g, "\n").normalize("NFC");
            if (!object.text.trim()) throw new Error("Enter some text before adding it.");
            // No shaping fallback: unsupported scripts are rejected instead of
            // silently producing missing glyphs or incorrectly shaped text.
            if (!/^[\u0020-\u007e\u00a0-\u024f\u1e00-\u1eff\u2000-\u206f\u20ac\n]+$/u.test(object.text)
                || /[\u200b-\u200f\u2028-\u202e\u2060-\u206f]/u.test(object.text))
                throw new Error("This preview supports Latin text, including Vietnamese, and common punctuation.");
            if (!Number.isFinite(value.fontSize) || value.fontSize < 6 || value.fontSize > 144)
                throw new Error("Choose a text size between 6 and 144 points.");
            object.fontSize = value.fontSize;
        } else {
            if (!Number.isFinite(value.strokeWidth) || value.strokeWidth < 0.5 || value.strokeWidth > 12)
                throw new Error("Choose a line width between 0.5 and 12 points.");
            object.strokeWidth = value.strokeWidth;
        }
        return object;
    }

    validateGeometry(value) {
        for (const key of ["x", "y", "width", "height"]) {
            if (!Number.isFinite(value[key]) || value[key] < 0 || value[key] > 1)
                throw new Error("Place the complete object inside the page.");
        }
        if (value.width < 0.00001 || value.height < 0.00001
            || value.x + value.width > 1.000001 || value.y + value.height > 1.000001)
            throw new Error("Place the complete object inside the page.");
    }

    async edit(command) {
        if (!this.canEdit) throw new Error(this.editReason);
        if (!command || typeof command.action !== "string") throw new Error("Invalid editing command.");
        let objects = this.objects;
        let selectedId = this.selectedId;
        let index = this.historyIndex;
        let history = this.history;
        let nextId = this.nextId;
        const action = command.action;
        if (action === "undo" || action === "redo") {
            index += action === "undo" ? -1 : 1;
            if (index < 0 || index >= history.length) throw new Error("There is no change to undo or redo.");
            objects = history[index];
            selectedId = 0;
        } else if (action === "discard") {
            objects = this.savedObjects;
            history = [objects];
            index = 0;
            selectedId = 0;
        } else {
            if (action === "add") {
                if (objects.length >= 128) throw new Error("This preview supports up to 128 added objects per PDF.");
                const object = this.validate(command.object, nextId++);
                objects = [...objects, object];
                selectedId = object.id;
            } else {
                const at = objects.findIndex(object => object.id === command.object?.id);
                const entry = this.original.entries.get(command.object?.id);
                const previous = at >= 0 ? objects[at] : entry?.object;
                if (!previous || previous.deleted) throw new Error("Select an editable object on a visible PDF page.");
                if (entry && !entry.object.editable) throw new Error(entry.object.readOnlyReason);
                if (at < 0 && objects.length >= 128) throw new Error("This preview supports up to 128 changed objects per PDF.");
                if (action === "update" || action === "replaceImage") {
                    let object;
                    if (action === "replaceImage") {
                        if (!entry || previous.type !== "image") throw new Error("Select an original image to replace.");
                        const image = command.image;
                        if (!image || !Number.isInteger(image.width) || !Number.isInteger(image.height)
                            || image.width < 1 || image.height < 1 || image.width > 4096 || image.height > 4096
                            || image.width*image.height > 4000000 || !(image.bytes instanceof ArrayBuffer)
                            || image.bytes.byteLength !== image.width*image.height*4)
                            throw new Error("Replacement images must be valid RGBA images up to 4 million pixels.");
                        const retained = [...this.images.values()].reduce((sum,value)=>sum+value.bytes.byteLength,0);
                        if (retained+image.bytes.byteLength > 32*1024*1024)
                            throw new Error("Replacement images and their undo history are limited to 32 MiB.");
                        const imageRevision = this.nextImageRevision;
                        this.images.set(imageRevision,{width:image.width,height:image.height,bytes:new Uint8Array(image.bytes)});
                        object = {...previous, imageRevision, hasReplacement:true};
                    } else object = entry ? this.original.validate(command.object,entry,previous)
                        : this.validate(command.object,previous.id);
                    if (object.type !== previous.type || object.page !== previous.page)
                        throw new Error("An existing object's type and page cannot be changed.");
                    objects = at >= 0 ? objects.map((value,i)=>i===at?object:value) : [...objects,object];
                    selectedId = object.id;
                } else if (action === "remove") {
                    objects = objects.filter((_, i) => i !== at);
                    if (entry) objects = [...objects,{id:previous.id,page:previous.page,source:true,deleted:true}];
                    selectedId = 0;
                } else throw new Error("Unsupported editing command.");
            }
            if (objects.length > 128) throw new Error("This preview supports up to 128 changed objects per PDF.");
            history = [...history.slice(0, index + 1), objects].slice(-32);
            index = history.length - 1;
        }
        let replacement;
        try { replacement = await this.build(objects); }
        catch (error) { this.pruneImages(); throw error; }
        this.objects = objects;
        this.selectedId = selectedId;
        this.history = history;
        this.historyIndex = index;
        this.nextId = nextId;
        if (action === "replaceImage") ++this.nextImageRevision;
        this.pruneImages();
        return replacement;
    }

    pruneImages() {
        const used = new Set([...this.history.flat(),...this.savedObjects,...this.objects].map(object=>object.imageRevision));
        for (const revision of this.images.keys()) if (!used.has(revision)) this.images.delete(revision);
    }

    async build(objects) {
        const m = this.engine;
        const fontData = objects.some(object => object.type === "text" && (!object.source
            || this.original.textChanged(object,this.original.entries.get(object.id).object))) ? await TrePdfEditor.font() : null;
        let bytes = 0, document = 0, font = 0, fontPointer = 0;
        try {
            bytes = m._malloc(this.source.length);
            if (!bytes) throw new Error("There is not enough browser memory to apply this change.");
            m.HEAPU8.set(this.source, bytes);
            document = m._FPDF_LoadMemDocument(bytes, this.source.length, 0);
            if (!document) throw new Error("The source PDF could not be prepared for editing.");
            if (fontData) {
                fontPointer = m._malloc(fontData.bytes.length);
                if (!fontPointer) throw new Error("There is not enough browser memory to load the text font.");
                m.HEAPU8.set(fontData.bytes, fontPointer);
                font = m._FPDFText_LoadFont(document, fontPointer, fontData.bytes.length, 2, 1);
                if (!font) throw new Error("The editing font could not be loaded.");
            }
            const pages = [...new Set(objects.map(object => object.page))];
            for (const pageIndex of pages) {
                const page = m._FPDF_LoadPage(document, pageIndex);
                if (!page) throw new Error("The PDF page could not be prepared for editing.");
                try {
                    const changes = objects.filter(object => object.page === pageIndex);
                    // Highest source indices first: removals/replacements never
                    // shift the still-to-be-applied lower source occurrences.
                    for (const object of changes.filter(value=>value.source).sort((a,b)=>b.id-a.id)) {
                        const entry=this.original.entries.get(object.id);
                        const handle=entry && m._FPDFPage_GetObject(page,entry.index);
                        if (!handle || m._FPDFPageObj_GetType(handle) !== (entry.object.type === "text" ? 1 : 3))
                            throw new Error("Original object validation failed. Your previous changes are retained.");
                        const replaceText = !object.deleted && object.type === "text" && this.original.textChanged(object,entry.object);
                        const replaceImage = !object.deleted && object.type === "image" && object.imageRevision;
                        if (object.deleted || replaceText || replaceImage) {
                            if (!m._FPDFPage_RemoveObject(page,handle)) throw new Error("The original object could not be removed.");
                            m._FPDFPageObj_Destroy(handle);
                            if (replaceText) this.addText(document,page,object,font,fontData,entry.index);
                            else if (replaceImage) {
                                const pixels=this.images.get(object.imageRevision);
                                if (!pixels) throw new Error("The replacement image is unavailable.");
                                const image=this.original.replaceImage(document,page,entry,object,pixels);
                                // Insertion transfers ownership even on failure.
                                if (!m._FPDFPage_InsertObjectAtIndex(page,image,entry.index))
                                    throw new Error("The replacement image could not be inserted.");
                            }
                        } else this.original.apply(page,handle,entry,object);
                    }
                    for (const object of changes.filter(value=>!value.source)) {
                        if (object.type === "text") this.addText(document, page, object, font, fontData);
                        else this.addShape(page, object);
                    }
                    if (!m._FPDFPage_GenerateContent(page)) throw new Error("The PDF change could not be generated.");
                } finally { m._FPDF_ClosePage(page); }
            }
            const result = { document, bytes };
            document = 0; bytes = 0;
            return result;
        } finally {
            if (font) m._FPDFFont_Close(font);
            if (fontPointer) m._free(fontPointer);
            if (document) m._FPDF_CloseDocument(document);
            if (bytes) m._free(bytes);
        }
    }

    point(page, x, y) {
        const m = this.engine;
        const pointer = m._malloc(16);
        if (!pointer) throw new Error("There is not enough browser memory to position this object.");
        try {
            if (!m._FPDF_DeviceToPage(page, 0, 0, 1000000, 1000000, 0,
                Math.round(x * 1000000), Math.round(y * 1000000), pointer, pointer + 8))
                throw new Error("The page coordinate system is unsupported.");
            return [m.HEAPF64[pointer / 8], m.HEAPF64[pointer / 8 + 1]];
        } finally { m._free(pointer); }
    }

    addShape(page, object) {
        const m = this.engine;
        const point = (x, y) => this.point(page, object.x + x * object.width, object.y + y * object.height);
        let handle = 0;
        const line = (x, y) => {
            const p = point(x, y);
            if (!handle) handle = m._FPDFPageObj_CreateNewPath(...p);
            else if (!m._FPDFPath_LineTo(handle, ...p)) throw new Error("The shape could not be drawn.");
            if (!handle) throw new Error("The shape could not be created.");
        };
        try {
            if (object.type === "ellipse") {
                const k = 0.2761423749;
                line(1, 0.5);
                for (const coordinates of [[1, .5+k, .5+k, 1, .5, 1], [.5-k, 1, 0, .5+k, 0, .5],
                    [0, .5-k, .5-k, 0, .5, 0], [.5+k, 0, 1, .5-k, 1, .5]]) {
                    const controls = [];
                    for (let i = 0; i < coordinates.length; i += 2) controls.push(...point(coordinates[i], coordinates[i+1]));
                    if (!m._FPDFPath_BezierTo(handle, ...controls)) throw new Error("The ellipse could not be drawn.");
                }
            } else if (object.type === "line" || object.type === "arrow") {
                line(0, 0); line(1, 1);
                if (object.type === "arrow") {
                    // Construct the arrowhead in physical display coordinates so
                    // a narrow or wide bounding box cannot distort its angle.
                    const size = this.pages[object.page];
                    const w = object.width * size.width, h = object.height * size.height;
                    const length = Math.hypot(w, h);
                    const head = Math.min(12, length * .28);
                    const dx = w / length, dy = h / length;
                    const clamp = value => Math.max(0, Math.min(1, value));
                    const a = point(clamp(1 - (dx * head + dy * head * .45) / w),
                        clamp(1 - (dy * head - dx * head * .45) / h));
                    const b = point(clamp(1 - (dx * head - dy * head * .45) / w),
                        clamp(1 - (dy * head + dx * head * .45) / h));
                    if (!m._FPDFPath_MoveTo(handle, ...a) || !m._FPDFPath_LineTo(handle, ...point(1, 1))
                        || !m._FPDFPath_LineTo(handle, ...b)) throw new Error("The arrow could not be drawn.");
                }
            } else {
                const vertices = object.type === "triangle" ? [[.5, 0], [1, 1], [0, 1]]
                    : object.type === "diamond" ? [[.5, 0], [1, .5], [.5, 1], [0, .5]]
                        : [[0, 0], [1, 0], [1, 1], [0, 1]];
                vertices.forEach(p => line(...p));
            }
            if (!["line", "arrow"].includes(object.type) && !m._FPDFPath_Close(handle))
                throw new Error("The shape outline could not be closed.");
            const rgb = TrePdfEditor.color(object.color);
            if (!m._FPDFPageObj_SetStrokeColor(handle, ...rgb, 255)
                || !m._FPDFPageObj_SetStrokeWidth(handle, object.strokeWidth)
                || !m._FPDFPath_SetDrawMode(handle, 0, 1)) throw new Error("The shape style could not be applied.");
            const inserted = m._FPDFPage_InsertObject(page, handle);
            handle = 0; // PDFium owns/frees the object even when insertion fails.
            if (!inserted) throw new Error("The shape could not be added to this page.");
        } finally { if (handle) m._FPDFPageObj_Destroy(handle); }
    }

    addText(document, page, object, font, data, insertIndex = null) {
        const m = this.engine;
        const size = this.pages[object.page];
        const width = object.width * size.width;
        const height = object.height * size.height;
        const advance = char => {
            const glyph = data.glyph(char.charCodeAt(0));
            if (!glyph) throw new Error("The editing font does not support one of these characters.");
            return data.advance(glyph) / data.units * object.fontSize;
        };
        const lines = [];
        for (const paragraph of object.text.split("\n")) {
            let line = "", lineWidth = 0;
            // Prefer word boundaries; split a single long word only when needed.
            for (const token of paragraph.match(/\S+|[^\S\n]+/gu) || [""]) {
                const tokenWidth = [...token].reduce((sum, char) => sum + advance(char), 0);
                if (line && lineWidth + tokenWidth > width) {
                    lines.push(line.trimEnd()); line = ""; lineWidth = 0;
                    if (!token.trim()) continue;
                }
                for (const char of token) {
                    const charWidth = advance(char);
                    if (charWidth > width) throw new Error("The text box is too narrow for this font size.");
                    if (lineWidth + charWidth > width && line) {
                        lines.push(line); line = ""; lineWidth = 0;
                    }
                    line += char; lineWidth += charWidth;
                }
            }
            lines.push(line);
        }
        const ascent = data.ascent / data.units * object.fontSize;
        const descent = -data.descent / data.units * object.fontSize;
        const spacing = Math.max(object.fontSize * 1.2, ascent + descent);
        if ((lines.length - 1) * spacing + ascent + descent > height + .001)
            throw new Error("The text does not fit. Increase the text box height or reduce the font size.");
        const rgb = TrePdfEditor.color(object.color);
        lines.forEach((line, lineIndex) => {
            if (!line.trim()) return;
            let handle = 0, textPointer = 0;
            try {
                handle = m._FPDFPageObj_CreateTextObj(document, font, object.fontSize);
                textPointer = m._malloc((line.length + 1) * 2);
                if (!handle || !textPointer) throw new Error("There is not enough browser memory to add text.");
                for (let i = 0; i <= line.length; ++i) m.HEAPU16[textPointer / 2 + i] = line.charCodeAt(i) || 0;
                if (!m._FPDFText_SetText(handle, textPointer) || !m._FPDFPageObj_SetFillColor(handle, ...rgb, 255))
                    throw new Error("The text could not be created.");
                const baseline = object.y + (ascent + lineIndex * spacing) / size.height;
                const origin = this.point(page, object.x, baseline);
                const right = this.point(page, object.x + 1 / size.width, baseline);
                const up = this.point(page, object.x, baseline - 1 / size.height);
                m._FPDFPageObj_Transform(handle, right[0]-origin[0], right[1]-origin[1],
                    up[0]-origin[0], up[1]-origin[1], ...origin);
                const inserted = insertIndex === null ? m._FPDFPage_InsertObject(page, handle)
                    : m._FPDFPage_InsertObjectAtIndex(page, handle, insertIndex++);
                handle = 0;
                if (!inserted) throw new Error("The text could not be added to this page.");
            } finally {
                if (textPointer) m._free(textPointer);
                if (handle) m._FPDFPageObj_Destroy(handle);
            }
        });
    }

    export(document) {
        if (!this.canEdit) throw new Error(this.editReason);
        const m = this.engine;
        const chunks = [];
        let length = 0, callback = 0, writer = 0, overflow = false;
        try {
            callback = m.addFunction((_, pointer, size) => {
                if (!Number.isSafeInteger(size) || size < 0 || length + size > 64 * 1024 * 1024) {
                    overflow = true; return 0;
                }
                chunks.push(m.HEAPU8.slice(pointer, pointer + size));
                length += size;
                return 1;
            }, "iiii");
            writer = m._malloc(8);
            if (!writer) throw new Error("There is not enough browser memory to export this PDF.");
            m.HEAP32[writer / 4] = 1;
            m.HEAP32[writer / 4 + 1] = callback;
            if (!m._FPDF_SaveAsCopy(document, writer, 2) || !length || overflow)
                throw new Error(overflow ? "The edited PDF exceeds the 64 MiB export limit." : "The PDF could not be exported.");
        } finally {
            if (writer) m._free(writer);
            if (callback) m.removeFunction(callback);
        }
        const result = new Uint8Array(length);
        let offset = 0;
        for (const chunk of chunks) { result.set(chunk, offset); offset += chunk.length; }
        let pointer = 0, reopened = 0;
        try {
            pointer = m._malloc(length);
            if (!pointer) throw new Error("There is not enough browser memory to verify the exported PDF.");
            m.HEAPU8.set(result, pointer);
            reopened = m._FPDF_LoadMemDocument(pointer, length, 0);
            if (!reopened || m._FPDF_GetPageCount(reopened) !== this.pages.length)
                throw new Error("The exported PDF did not pass validation. Your changes are still available.");
        } finally {
            if (reopened) m._FPDF_CloseDocument(reopened);
            if (pointer) m._free(pointer);
        }
        this.savedObjects = this.objects;
        this.pruneImages();
        return result.buffer;
    }

    static color(value) { return [1, 3, 5].map(offset => parseInt(value.slice(offset, offset + 2), 16)); }

    static async font() {
        if (!this.fontPromise) {
            this.fontPromise = (async () => {
                const response = await fetch(new URL("fonts/LiberationSans-Regular.ttf", self.location.href));
                if (!response.ok) throw new Error("The editing font could not be downloaded. Try again.");
                const bytes = new Uint8Array(await response.arrayBuffer());
                if (bytes.length < 1024 || bytes.length > 2 * 1024 * 1024) throw new Error("The editing font is invalid.");
                const view = new DataView(bytes.buffer);
                const tables = {};
                for (let i = 0; i < view.getUint16(4); ++i) {
                    const at = 12 + 16 * i;
                    const name = String.fromCharCode(...bytes.slice(at, at + 4));
                    tables[name] = view.getUint32(at + 8);
                }
                const cmap = tables.cmap;
                let subtable = 0;
                for (let i = 0; i < view.getUint16(cmap + 2); ++i) {
                    const at = cmap + 4 + i * 8;
                    const offset = cmap + view.getUint32(at + 4);
                    if (view.getUint16(offset) === 4 && [0, 3].includes(view.getUint16(at))) subtable = offset;
                }
                if (!subtable || !tables.head || !tables.hhea || !tables.hmtx) throw new Error("The editing font metrics are invalid.");
                const segments = view.getUint16(subtable + 6) / 2;
                const end = subtable + 14, start = end + segments * 2 + 2;
                const delta = start + segments * 2, ranges = delta + segments * 2;
                const metrics = view.getUint16(tables.hhea + 34);
                return { bytes, units: view.getUint16(tables.head + 18),
                    ascent: view.getInt16(tables.hhea + 4), descent: view.getInt16(tables.hhea + 6),
                    advance: glyph => view.getUint16(tables.hmtx + Math.min(glyph, metrics - 1) * 4),
                    glyph: code => {
                        for (let i = 0; i < segments; ++i) {
                            if (code > view.getUint16(end + i * 2)) continue;
                            if (code < view.getUint16(start + i * 2)) return 0;
                            const adjustment = view.getInt16(delta + i * 2);
                            const range = view.getUint16(ranges + i * 2);
                            if (!range) return (code + adjustment) & 0xffff;
                            const glyph = view.getUint16(ranges + i * 2 + range + 2 * (code - view.getUint16(start + i * 2)));
                            return glyph ? (glyph + adjustment) & 0xffff : 0;
                        }
                        return 0;
                    } };
            })().catch(error => { this.fontPromise = null; throw error; });
        }
        return this.fontPromise;
    }
}
