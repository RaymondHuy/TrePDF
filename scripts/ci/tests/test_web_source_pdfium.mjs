// Exercises real PDFium WASM editing, transactional failures and saved PDFs.
// node scripts/ci/tests/test_web_edit_pdfium.mjs /path/to/pdfium/lib
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Worker } from "node:worker_threads";

const root = fileURLToPath(new URL("../../../", import.meta.url));
const engineDirectory = path.resolve(process.argv[2] || "_deps/web/pdfium/lib");
const wrapper = `
const { parentPort, workerData } = require('node:worker_threads');
const testFs = require('node:fs'), vm = require('node:vm'), path = require('node:path');
const { pathToFileURL } = require('node:url');
global.self = global; global.require = require; global.__dirname = workerData.engineDirectory;
global.location = { href: pathToFileURL(path.join(workerData.engineDirectory, 'pdfium-worker.js')).href };
global.postMessage = (value, transfers) => parentPort.postMessage(value, transfers);
global.fetch = async url => {
    if (!String(url).endsWith('/fonts/LiberationSans-Regular.ttf')) throw new Error('Unexpected network request');
    const bytes = testFs.readFileSync(path.join(workerData.root, 'tests/fixtures/fonts/liberation/LiberationSans-Regular.ttf'));
    return { ok: true, arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) };
};
global.importScripts = name => {
    if (!['./pdfium.js', './pdfium-editor.js', './pdfium-source.js'].includes(name)) throw new Error('Unexpected worker import');
    const base = name === './pdfium.js' ? workerData.engineDirectory : path.join(workerData.root, 'src/web');
    vm.runInThisContext(testFs.readFileSync(path.join(base, name), 'utf8'));
};
parentPort.on('message', request => {
    if (request.type === 'retainedImages') {
        const result=vm.runInThisContext('({ count:editor.images.size, bytes:[...editor.images.values()].reduce((n,v)=>n+v.bytes.byteLength,0) })');
        self.postMessage({type:'retainedImages',operationId:request.operationId,...result});return;
    }
    if (request.type === 'failNextInsert') {
        const original = self.Module._FPDFPage_InsertObjectAtIndex;
        self.Module._FPDFPage_InsertObjectAtIndex = (page,object,index) => {
            self.Module._FPDFPage_InsertObjectAtIndex = original;
            // Use the real ownership-consuming failure path (out-of-bounds index).
            return original(page,object,0x7fffffff);
        };
        self.postMessage({ type: 'failureInjected', operationId: request.operationId }); return;
    }
    if (request.type === 'failNextGenerate') {
        const original = self.Module._FPDFPage_GenerateContent;
        self.Module._FPDFPage_GenerateContent = (...args) => { self.Module._FPDFPage_GenerateContent = original; return 0; };
        self.postMessage({ type: 'failureInjected', operationId: request.operationId }); return;
    }
    if (request.type === 'failNextExport') {
        const original = self.Module._FPDF_SaveAsCopy;
        self.Module._FPDF_SaveAsCopy = (...args) => { self.Module._FPDF_SaveAsCopy = original; return 0; };
        self.postMessage({ type: 'failureInjected', operationId: request.operationId });
        return;
    }
    if (request.type !== 'inspect') { self.onmessage({ data: request }); return; }
    const m = self.Module, bytes = new Uint8Array(request.bytes), pointer = m._malloc(bytes.length);
    m.HEAPU8.set(bytes, pointer);
    const document = m._FPDF_LoadMemDocument(pointer, bytes.length, 0);
    const page = m._FPDF_LoadPage(document, 0), textPage = m._FPDFText_LoadPage(page);
    const count = m._FPDFText_CountChars(textPage), textPointer = m._malloc((count + 1) * 2);
    const actual = m._FPDFText_GetText(textPage, 0, count, textPointer);
    const text = String.fromCharCode(...m.HEAPU16.slice(textPointer / 2, textPointer / 2 + Math.max(0, actual - 1)));
    const objects = [];
    for (let i = 0; i < m._FPDFPage_CountObjects(page); ++i) {
        const object = m._FPDFPage_GetObject(page, i);
        const scratch=m._malloc(24);
        m._FPDFPageObj_GetMatrix(object,scratch);
        const matrix=Array.from(m.HEAPF32.slice(scratch/4,scratch/4+6));
        let fontName='', imagePixels=[], placementSize=[];
        if (m._FPDFPageObj_GetType(object)===1) {
            const font=m._FPDFTextObj_GetFont(object), size=m._FPDFFont_GetBaseFontName(font,0,0), name=m._malloc(size);
            m._FPDFFont_GetBaseFontName(font,name,size); fontName=new TextDecoder().decode(m.HEAPU8.slice(name,name+size-1)); m._free(name);
        } else if (m._FPDFPageObj_GetType(object)===3) {
            const bitmap=m._FPDFImageObj_GetBitmap(object);
            if (bitmap) { const p=m._FPDFBitmap_GetBuffer(bitmap); imagePixels=Array.from(m.HEAPU8.slice(p,p+12)); m._FPDFBitmap_Destroy(bitmap); }
            const rendered=m._FPDFImageObj_GetRenderedBitmap(document,page,object);
            if (rendered) {
                const w=m._FPDFBitmap_GetWidth(rendered),h=m._FPDFBitmap_GetHeight(rendered);
                placementSize=[w,h];
                m._FPDFBitmap_Destroy(rendered);
            }
        }
        m._free(scratch);
        objects.push({ type: m._FPDFPageObj_GetType(object), matrix, fontName, imagePixels, placementSize,
            segments: m._FPDFPageObj_GetType(object) === 2 ? m._FPDFPath_CountSegments(object) : 0 });
    }
    m._free(textPointer); m._FPDFText_ClosePage(textPage); m._FPDF_ClosePage(page);
    m._FPDF_CloseDocument(document); m._free(pointer);
    self.postMessage({ type: 'inspected', operationId: request.operationId, text, objects });
});
vm.runInThisContext(testFs.readFileSync(path.join(workerData.root, 'src/web/pdfium-worker.js'), 'utf8'));
`;

function fixture({clip=false,mask=false,softMask=false,nested=false,rotate=0,crop=false,tinyMask=false}={}) {
    const text = "BT /F1 18 Tf 60 320 Td (Original source text) Tj ET\n";
    const imageSide = tinyMask ? 256 : 2;
    const placement = tinyMask ? "1 0 0 1 300 120" : "120 0 0 90 300 120";
    const content = (clip ? "q 80 130 300 195 re W n\n" : "") + text
        + `q ${placement} cm /Im1 Do Q\n0 0 1 rg 550 20 10 10 re f\n`
        + (clip ? "Q\n" : "") + (nested ? "/Fm1 Do\n" : "");
    const image = tinyMask ? "ff0000".repeat(imageSide*imageSide) : mask ? "80" : "ff000000ff000000ffffffff";
    const alpha = tinyMask ? "ff".repeat(imageSide*imageSide-1)+"00" : "80ff80ff";
    const objects=[
        "<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 400] /Rotate ${rotate} ${crop ? "/CropBox [30 20 570 380]" : ""} /Resources << /Font << /F1 5 0 R >> /XObject << /Im1 6 0 R /Fm1 7 0 R >> >> /Contents 4 0 R >>`,
        `<< /Length ${content.length} >>\nstream\n${content}endstream`,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        `<< /Type /XObject /Subtype /Image /Width ${imageSide} /Height ${imageSide} ${mask ? "/ImageMask true /BitsPerComponent 1" : "/ColorSpace /DeviceRGB /BitsPerComponent 8"} ${softMask || tinyMask ? "/SMask 8 0 R" : ""} /Filter /ASCIIHexDecode /Length ${image.length+1} >>\nstream\n${image}>\nendstream`,
        `<< /Type /XObject /Subtype /Form /BBox [0 0 600 400] /Resources << /Font << /F1 5 0 R >> >> /Length ${text.length} >>\nstream\n${text}endstream`,
        `<< /Type /XObject /Subtype /Image /Width ${imageSide} /Height ${imageSide} /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /ASCIIHexDecode /Length ${alpha.length+1} >>\nstream\n${alpha}>\nendstream`
    ];
    let pdf="%PDF-1.7\n"; const offsets=[0];
    objects.forEach((object,index)=>{offsets.push(pdf.length);pdf+=`${index+1} 0 obj\n${object}\nendobj\n`;});
    const start=pdf.length; pdf+=`xref\n0 ${objects.length+1}\n0000000000 65535 f \n`;
    for(const offset of offsets.slice(1))pdf+=`${String(offset).padStart(10,"0")} 00000 n \n`;
    pdf+=`trailer\n<< /Size ${objects.length+1} /Root 1 0 R >>\nstartxref\n${start}\n%%EOF\n`;
    return new TextEncoder().encode(pdf).buffer;
}
const worker = new Worker(wrapper, { eval: true, workerData: { engineDirectory, root } });
const messages = [];
let notify, workerError;
worker.on("message", message => { messages.push(message); notify?.(); });
worker.on("error", error => { workerError = error; notify?.(); });
async function receive(predicate) {
    const deadline = Date.now() + 15000;
    for (;;) {
        if (workerError) throw workerError;
        const index = messages.findIndex(predicate);
        if (index >= 0) return messages.splice(index, 1)[0];
        if (Date.now() > deadline) throw new Error("PDF worker timeout: " + JSON.stringify(messages));
        await new Promise(resolve => {
            const timer = setTimeout(resolve, 100);
            notify = () => { clearTimeout(timer); resolve(); };
        });
    }
}
let operationId = 0, generation = 1;
async function send(request) {
    request = { generation, operationId: ++operationId, ...request };
    worker.postMessage(request, request.bytes ? [request.bytes] : []);
    return receive(message => request.type === "open" ? message.generation === request.generation
        : request.type === "render" ? message.type !== "indexed" && message.requestId === request.requestId
            : message.operationId === request.operationId);
}
const edit = command => send({ type: "edit", command });

async function open(bytes=fixture()) {
    ++generation;
    const opened=await send({type:"open",bytes}); assert.equal(opened.type,"opened",opened.message);
    const rendered=await send({type:"render",requestId:operationId+1,page:0,width:600}); assert.equal(rendered.type,"rendered",rendered.message);
    return receive(message=>message.type==="indexed" && message.generation===generation);
}
const inspect = async()=>{const result=await send({type:"export"});assert.equal(result.type,"exported",result.message);return send({type:"inspect",bytes:result.bytes});};
try {
    await receive(message=>message.type==="ready");
    let state=await open();
    assert.equal(state.objects.length,2);
    const original=state.objects.find(object=>object.type==="text"), image=state.objects.find(object=>object.type==="image");
    assert.equal(original.editable,true,JSON.stringify(original));assert.equal(image.editable,true,JSON.stringify(image));
    assert.equal(original.text,"Original source text"); assert.equal(original.source,true);assert.ok(original.id>=1000000);
    const originalFile=await send({type:"inspect",bytes:fixture()});
    state=await edit({action:"update",object:{...original,x:.15}});assert.equal(state.type,"edited",state.message);
    let file=await inspect();assert.match(file.text,/Original source text/);assert.equal(file.objects[0].fontName,"Helvetica");
    assert.ok(Math.abs(file.objects[0].matrix[4]-originalFile.objects[0].matrix[4]-(.15-original.x)*600)<.02,"source text moved physically");
    await send({type:"failNextInsert"});
    state=await edit({action:"update",object:{...original,text:"Xin chào Việt Nam",width:.65,height:.15}});
    assert.equal(state.type,"error");file=await inspect();assert.match(file.text,/Original source text/,"failed insertion preserves previous PDF without double-free");
    state=await edit({action:"update",object:{...original,text:"Xin chào Việt Nam",width:.65,height:.15}});
    assert.equal(state.type,"edited",state.message);file=await inspect();
    assert.doesNotMatch(file.text,/Original source text/);assert.match(file.text,/Xin chào Việt Nam/);
    assert.deepEqual(file.objects.map(object=>object.type),[1,3,2],"text replacement retains source z-order");
    assert.match(file.objects[0].fontName,/LiberationSans/);
    state=await edit({action:"undo"});assert.equal(state.type,"edited");file=await inspect();assert.match(file.text,/Original source text/);
    state=await edit({action:"redo"});assert.equal(state.type,"edited");
    state=await edit({action:"update",object:{...image,x:.2,y:.5,width:.3,height:.2,quarterTurns:1}});
    assert.equal(state.type,"edited",state.message);file=await inspect();
    assert.equal(file.objects[1].type,3);assert.deepEqual(file.objects[1].imagePixels,originalFile.objects[1].imagePixels,"geometry retains source pixels");
    assert.ok(Math.abs(file.objects[1].matrix[0])<.01);assert.ok(Math.abs(file.objects[1].matrix[1]+80)<.02);
    state=await edit({action:"replaceImage",object:{id:image.id},image:{width:2,height:2,bytes:new Uint8Array([20,40,200,255,20,40,200,255,20,40,200,255,20,40,200,255]).buffer}});
    assert.equal(state.type,"edited",state.message);assert.equal(state.objects.find(o=>o.id===image.id).hasReplacement,true);
    assert.equal(JSON.stringify(state).includes('bytes'),false,"pixels never enter public state");
    file=await inspect();assert.deepEqual(file.objects.map(o=>o.type),[1,3,2]);assert.notDeepEqual(file.objects[1].imagePixels,originalFile.objects[1].imagePixels);
    await send({type:"failNextInsert"});
    state=await edit({action:"replaceImage",object:{id:image.id},image:{width:1,height:1,bytes:new Uint8Array([255,0,0,255]).buffer}});
    assert.equal(state.type,"error");file=await inspect();assert.equal(file.objects[1].type,3,"failed image insertion preserves previous PDF");
    await send({type:"failNextGenerate"});
    state=await edit({action:"remove",object:{id:image.id}});assert.equal(state.type,"error");file=await inspect();assert.equal(file.objects[1].type,3,"failure retains image");
    state=await edit({action:"remove",object:{id:image.id}});assert.equal(state.type,"edited");file=await inspect();assert.deepEqual(file.objects.map(o=>o.type),[1,2]);
    state=await edit({action:"undo"});assert.equal(state.type,"edited");file=await inspect();assert.equal(file.objects[1].type,3);
    state=await edit({action:"remove",object:{id:original.id}});assert.equal(state.type,"edited");
    state=await edit({action:"discard"});assert.equal(state.type,"edited");assert.ok(state.objects.find(o=>o.id===original.id),"discard restores last exported source edits");
    const saved=await send({type:"export"});state=await open(saved.bytes);assert.ok(state.objects.some(o=>o.text==="Xin chào Việt Nam"),"saved source replacement reopens as editable source");
    state=await open(fixture({clip:true,nested:true}));
    assert.equal(state.objects.length,2,"nested objects excluded");assert.ok(state.objects.every(o=>!o.editable));
    const rejected=await edit({action:"remove",object:{id:state.objects[0].id}});assert.equal(rejected.type,"error");assert.match(rejected.message,/Clipped/);
    state=await open(fixture({mask:true}));assert.equal(state.objects.find(o=>o.type==="image").editable,false,"stencil mask read-only");
    state=await open(fixture({softMask:true})); assert.equal(state.objects.find(o=>o.type==="image").editable,false,"soft mask read-only");
    const tiny = await send({type:"inspect",bytes:fixture({tinyMask:true})});
    assert.deepEqual(tiny.objects.find(o=>o.type===3).placementSize,[1,1],"fixture placement raster is much smaller than its 256x256 intrinsic mask");
    state = await open(fixture({tinyMask:true}));
    assert.equal(state.objects.find(o=>o.type==="image").editable,false,"one transparent intrinsic pixel remains protected at a 1-point display size");
    for(const rotate of [0,90,180,270]) {
        state=await open(fixture({rotate,crop:true}));
        const source=state.objects.find(o=>o.type==="image");assert.equal(source.editable,true,source.readOnlyReason);
        state=await edit({action:"update",object:{...source,x:.1,y:.4,width:.2,height:.2}});assert.equal(state.type,"edited",state.message);
        const exported=await send({type:"export"});state=await open(exported.bytes);
        const moved=state.objects.find(o=>o.type==="image");
        for(const [key,value] of Object.entries({x:.1,y:.4,width:.2,height:.2}))
            assert.ok(Math.abs(moved[key]-value)<.00001,`source image ${key} survives crop/rotation ${rotate}`);
    }
    state=await open();
    const sourceImage=state.objects.find(o=>o.type==="image");
    for(let i=0;i<128;++i) {
        state=await edit({action:"add",object:{type:"rectangle",page:0,x:.1,y:.5,width:.2,height:.2,color:"#ff0000",strokeWidth:1}});
        assert.equal(state.type,"edited",state.message);
    }
    state=await edit({action:"replaceImage",object:{id:sourceImage.id},image:{width:1,height:1,bytes:new Uint8Array([255,0,0,255]).buffer}});
    assert.equal(state.type,"error");assert.match(state.message,/128/);
    const retained=await send({type:"retainedImages"});assert.equal(retained.bytes,0,"over-limit image replacement does not leak retained pixel buffers");
    state=await edit({action:"discard"});assert.equal(state.type,"edited");
    await send({type:"failNextGenerate"});
    state=await edit({action:"replaceImage",object:{id:sourceImage.id},image:{width:1,height:1,bytes:new Uint8Array([255,0,0,255]).buffer}});
    assert.equal(state.type,"error");assert.equal((await send({type:"retainedImages"})).bytes,0,"failed image generation does not leak retained pixels");
    console.log("PDFium source edit: source text replacement/font-preserving move, source image move/resize/rotate/replace/delete, exact z-order, undo/redo/discard, saved reopen, failure atomicity and clipped/nested/stencil protections passed.");
} finally { await worker.terminate(); }
