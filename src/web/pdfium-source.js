/* Browser-only source indexing. Handles never escape a loaded pristine page.
 * Only bounded, opaque, unmarked top-level objects can be mutated. */
"use strict";
class TrePdfSource {
    constructor(editor) {
        this.editor = editor;
        this.entries = new Map();
        this.scanned = new Set();
        this.limited = false;
    }
    notice() {
        return this.limited ? "Some original objects are read-only: complex, clipped, masked, rotated text, nested content or indexing limits."
            : "Original text changes use Liberation Sans; moving text retains its original font. Scanned page images do not contain editable text.";
    }
    merged(changes) {
        const byId = new Map(changes.map(object => [object.id, object]));
        const result = [];
        for (const { object } of this.entries.values()) {
            const current = byId.get(object.id) || object;
            if (!current.deleted) result.push(current);
        }
        return [...result, ...changes.filter(object => !object.source)];
    }
    scan(pageIndex) {
        if (this.scanned.has(pageIndex) || !this.editor.canEdit) return false;
        const m = this.editor.engine;
        let bytes = 0, document = 0, page = 0, textPage = 0;
        try {
            bytes = m._malloc(this.editor.source.length);
            if (!bytes) throw new Error("Not enough memory to index original content.");
            m.HEAPU8.set(this.editor.source, bytes);
            document = m._FPDF_LoadMemDocument(bytes, this.editor.source.length, 0);
            page = document && m._FPDF_LoadPage(document, pageIndex);
            if (!page) throw new Error("Original page content could not be indexed.");
            const count = m._FPDFPage_CountObjects(page);
            if (count < 0 || count > 2048 || this.entries.size >= 4096) {
                this.limited = true; this.scanned.add(pageIndex); return true;
            }
            textPage = m._FPDFText_LoadPage(page);
            if (textPage && m._FPDFText_CountChars(textPage) > 50000) {
                m._FPDFText_ClosePage(textPage); textPage = 0; this.limited = true;
            }
            this.imageDecodeBudget = 8000000;
            const results = [];
            for (let index = 0; index < count; ++index) {
                if (this.entries.size + results.length >= 4096) { this.limited = true; break; }
                const handle = m._FPDFPage_GetObject(page, index), type = m._FPDFPageObj_GetType(handle);
                if (![1, 3].includes(type)) { if (type === 5) this.limited = true; continue; }
                const entry = this.inspect(document, page, textPage, handle, pageIndex, index);
                if (entry) results.push(entry);
            }
            for (const entry of results) this.entries.set(entry.object.id, entry);
            this.scanned.add(pageIndex);
            return true;
        } finally {
            if (textPage) m._FPDFText_ClosePage(textPage);
            if (page) m._FPDF_ClosePage(page);
            if (document) m._FPDF_CloseDocument(document);
            if (bytes) m._free(bytes);
        }
    }
    displayPoint(page, x, y) {
        const m = this.editor.engine, p = m._malloc(8);
        if (!p) throw new Error("Not enough memory to read source geometry.");
        try {
            if (!m._FPDF_PageToDevice(page, 0, 0, 1000000, 1000000, 0, x, y, p, p + 4))
                throw new Error("Unsupported source geometry.");
            return [m.HEAP32[p / 4] / 1000000, m.HEAP32[p / 4 + 1] / 1000000];
        } finally { m._free(p); }
    }
    inspect(document, page, textPage, handle, pageIndex, index) {
        const m = this.editor.engine, p = m._malloc(64);
        if (!p) throw new Error("Not enough memory to read source content.");
        try {
            if (!m._FPDFPageObj_GetBounds(handle, p, p+4, p+8, p+12)) return null;
            const [left, bottom, right, top] = m.HEAPF32.slice(p/4, p/4+4);
            if (![left,bottom,right,top].every(Number.isFinite) || right <= left || top <= bottom) return null;
            const points = [[left,bottom],[left,top],[right,bottom],[right,top]].map(([x,y]) => this.displayPoint(page,x,y));
            const x = Math.min(...points.map(v=>v[0])), y = Math.min(...points.map(v=>v[1]));
            const width = Math.max(...points.map(v=>v[0]))-x, height = Math.max(...points.map(v=>v[1]))-y;
            if (x < -0.000001 || y < -0.000001 || x+width > 1.000001 || y+height > 1.000001) {
                this.limited = true; return null;
            }
            if (!m._FPDFPageObj_GetMatrix(handle,p)) return null;
            const matrix = Array.from(m.HEAPF32.slice(p/4,p/4+6));
            const [a,b,c,d,e,f] = matrix;
            if (!matrix.every(Number.isFinite) || Math.abs(a*d-b*c) < 1e-10) return null;
            const type = m._FPDFPageObj_GetType(handle) === 1 ? "text" : "image";
            const object = { id:1000000+pageIndex*4096+index, type, source:true, page:pageIndex,
                x:Math.max(0,x), y:Math.max(0,y), width, height, editable:true, readOnlyReason:"" };
            const reject = reason => { object.editable=false; object.readOnlyReason=reason; this.limited=true; };
            const clip = m._FPDFPageObj_GetClipPath(handle);
            if (!clip || m._FPDFClipPath_CountPaths(clip) !== -1) reject("Clipped original objects are read-only in this preview.");
            if (m._FPDFPageObj_HasTransparency(handle)) reject("Transparent or masked original objects are read-only in this preview.");
            if (m._FPDFPageObj_CountMarks(handle) !== 0) reject("Tagged or managed original objects are read-only in this preview.");
            if (type === "text") {
                if (!textPage) return null;
                const length = m._FPDFTextObj_GetText(handle,textPage,0,0);
                if (length < 4 || length > 2050 || length % 2) { this.limited=true; return null; }
                const buffer = m._malloc(length);
                if (!buffer) throw new Error("Not enough memory to read original text.");
                try {
                    if (m._FPDFTextObj_GetText(handle,textPage,buffer,length) !== length) return null;
                    object.text = String.fromCharCode(...m.HEAPU16.slice(buffer/2,buffer/2+length/2-1));
                } finally { m._free(buffer); }
                if (!object.text.trim()) return null;
                if (m._FPDFTextObj_GetTextRenderMode(handle) !== 0) reject("Outlined or invisible text is read-only in this preview.");
                const origin = this.displayPoint(page,e,f), rightPoint = this.displayPoint(page,e+a,f+b), up = this.displayPoint(page,e+c,f+d);
                if (rightPoint[0] <= origin[0] || up[1] >= origin[1]
                    || Math.abs(rightPoint[1]-origin[1]) > 0.000002 || Math.abs(up[0]-origin[0]) > 0.000002)
                    reject("Rotated or skewed original text is read-only in this preview.");
                if (!m._FPDFTextObj_GetFontSize(handle,p)) return null;
                object.fontSize = m.HEAPF32[p/4] * Math.hypot(c,d);
                if (object.fontSize < 6 || object.fontSize > 144) reject("This original text size is outside the supported range.");
                object.color = "#000000";
                if (!m._FPDFPageObj_GetFillColor(handle,p,p+4,p+8,p+12)) reject("This original text color cannot be edited safely.");
                else {
                    const rgba = Array.from(m.HEAPU32.slice(p/4,p/4+4));
                    object.color = "#" + rgba.slice(0,3).map(v=>v.toString(16).padStart(2,"0")).join("");
                    if (rgba[3] !== 255) reject("Transparent original text is read-only in this preview.");
                }
                object.fontSubstitutionRequired = true;
                const font = m._FPDFTextObj_GetFont(handle), size = font && m._FPDFFont_GetBaseFontName(font,0,0);
                object.fontFamily = "Original PDF font";
                if (size > 0 && size <= 256) {
                    const buffer = m._malloc(size);
                    if (!buffer) throw new Error("Not enough memory to read original font.");
                    try { m._FPDFFont_GetBaseFontName(font,buffer,size); object.fontFamily = new TextDecoder().decode(m.HEAPU8.slice(buffer,buffer+size-1)); }
                    finally { m._free(buffer); }
                }
                // Source glyph bounds are often shorter than one editable line.
                // This logical frame leaves the original baseline untouched for
                // geometry-only edits and gives replacement text a usable box.
                object.height = Math.min(1-object.y, Math.max(height, object.fontSize*1.25/this.editor.pages[pageIndex].height));
            } else {
                object.quarterTurns = 0;
                object.hasReplacement = false;
                object.imageRevision = 0;
                if (!m._FPDFImageObj_GetImageMetadata(handle,page,p)) reject("This image metadata cannot be verified.");
                else {
                    const [pixelWidth,pixelHeight] = m.HEAPU32.slice(p/4,p/4+2);
                    const bits = m.HEAPU32[p/4+4], space = m.HEAP32[p/4+5];
                    if (pixelWidth < 1 || pixelHeight < 1 || pixelWidth > 4096 || pixelHeight > 4096
                        || pixelWidth*pixelHeight > 4000000 || bits < 8 || ![1,2,3].includes(space))
                        reject("Large, stencil or special-color images are read-only in this preview.");
                }
                // HasTransparency() describes graphics state and does not report
                // an image's own /Mask or /SMask. Inspect at intrinsic resolution:
                // rendering a large image at its tiny page placement could round
                // a small transparent region to fully opaque pixels.
                if (object.editable) {
                    const pixelWidth = m.HEAPU32[p/4], pixelHeight = m.HEAPU32[p/4+1];
                    const cost = pixelWidth * pixelHeight;
                    if (cost > this.imageDecodeBudget) reject("This page exceeds the safe image inspection limit.");
                    else {
                        this.imageDecodeBudget -= cost;
                        let bitmap = 0;
                        if (!m._FPDFImageObj_SetMatrix(handle,pixelWidth,0,0,pixelHeight,0,0))
                            reject("This image cannot be inspected safely.");
                        else try {
                            bitmap = m._FPDFImageObj_GetRenderedBitmap(document,page,handle);
                            if (!bitmap) reject("This image cannot be decoded safely.");
                            else {
                                const w = m._FPDFBitmap_GetWidth(bitmap), h = m._FPDFBitmap_GetHeight(bitmap);
                                const format = m._FPDFBitmap_GetFormat(bitmap), stride = m._FPDFBitmap_GetStride(bitmap);
                                const data = m._FPDFBitmap_GetBuffer(bitmap);
                                if (w !== pixelWidth || h !== pixelHeight || w*h > 4000000 || !data || ![1,2,3,4].includes(format))
                                    reject("This image cannot be decoded safely.");
                                else if (format === 4) {
                                    outer: for (let row = 0; row < h; ++row) for (let col = 0; col < w; ++col)
                                        if (m.HEAPU8[data+row*stride+col*4+3] !== 255) {
                                            reject("Masked or transparent original images are read-only in this preview."); break outer;
                                        }
                                }
                            }
                        } finally {
                            if (bitmap) m._FPDFBitmap_Destroy(bitmap);
                            // This page belongs to the private indexing document;
                            // restore its matrix before releasing the scan anyway.
                            if (!m._FPDFImageObj_SetMatrix(handle,...matrix))
                                reject("This image's placement cannot be verified safely.");
                        }
                    }
                }
            }
            return { object, matrix, index };
        } finally { m._free(p); }
    }
    validate(value, entry, previous) {
        const original = entry.object;
        if (!original.editable) throw new Error(original.readOnlyReason);
        if (value.type !== original.type || value.page !== original.page) throw new Error("An original object's type and page cannot be changed.");
        const candidate = {...original, ...previous};
        for (const key of ["x","y","width","height"]) candidate[key] = value[key];
        this.editor.validateGeometry(candidate);
        if (original.type === "text") {
            for (const key of ["text","fontSize","color"]) candidate[key] = value[key];
            if (this.textChanged(candidate, original)) {
                const validated = this.editor.validate(candidate, original.id);
                Object.assign(candidate, validated);
                candidate.fontFamily = "Liberation Sans";
            } else candidate.fontFamily = original.fontFamily;
        } else {
            if (!Number.isInteger(value.quarterTurns) || value.quarterTurns < 0 || value.quarterTurns > 3)
                throw new Error("Choose a valid image rotation.");
            candidate.quarterTurns = value.quarterTurns;
        }
        return candidate;
    }
    textChanged(object, original) {
        return object.text !== original.text || object.fontSize !== original.fontSize || object.color !== original.color;
    }
    matrix(page, entry, object) {
        const [a,b,c,d,e,f] = entry.matrix;
        const base = entry.object;
        const map = (px,py) => {
            const display = this.displayPoint(page,px,py);
            let x=(display[0]-base.x)/base.width, y=(display[1]-base.y)/base.height;
            switch (object.quarterTurns || 0) {
                case 1: [x,y]=[1-y,x]; break;
                case 2: [x,y]=[1-x,1-y]; break;
                case 3: [x,y]=[y,1-x]; break;
            }
            return this.editor.point(page,object.x+x*object.width,object.y+y*object.height);
        };
        const origin=map(e,f), right=map(e+a,f+b), up=map(e+c,f+d);
        return [right[0]-origin[0],right[1]-origin[1],up[0]-origin[0],up[1]-origin[1],...origin];
    }
    apply(page, handle, entry, object) {
        const m=this.editor.engine, matrix=this.matrix(page,entry,object), p=m._malloc(24);
        if (!p) throw new Error("Not enough memory to transform original content.");
        try {
            m.HEAPF32.set(matrix,p/4);
            if (!m._FPDFPageObj_SetMatrix(handle,p)) throw new Error("The original object's position could not be changed.");
        } finally { m._free(p); }
    }
    replaceImage(document,page,entry,object,pixels) {
        const m=this.editor.engine;
        let handle=0, bitmap=0, buffer=0;
        try {
            buffer=m._malloc(pixels.bytes.length);
            if (!buffer) throw new Error("Not enough memory to replace this image.");
            const rgba=pixels.bytes;
            for (let i=0;i<rgba.length;i+=4) {
                m.HEAPU8[buffer+i]=rgba[i+2]; m.HEAPU8[buffer+i+1]=rgba[i+1];
                m.HEAPU8[buffer+i+2]=rgba[i]; m.HEAPU8[buffer+i+3]=rgba[i+3];
            }
            bitmap=m._FPDFBitmap_CreateEx(pixels.width,pixels.height,4,buffer,pixels.width*4);
            handle=m._FPDFPageObj_NewImageObj(document);
            if (!bitmap || !handle || !m._FPDFImageObj_SetBitmap(0,0,handle,bitmap)) throw new Error("The replacement image could not be created.");
            this.apply(page,handle,entry,object);
            const result=handle; handle=0; return result;
        } finally {
            if (handle) m._FPDFPageObj_Destroy(handle);
            if (bitmap) m._FPDFBitmap_Destroy(bitmap);
            if (buffer) m._free(buffer);
        }
    }
}
