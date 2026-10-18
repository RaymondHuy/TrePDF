// Included in the Qt runtime with --pre-js. The worker has a separate Wasm
// memory, keeping the native PDF engine off the browser's UI thread.
function createTrePDFBrowserBridge(runtime, browser) {
    const maximumFileBytes = 64 * 1024 * 1024;
    let worker = null;
    let generation = 0;
    let originalFile = null;
    let candidateFile = null;
    let opened = false;
    let picker = null;
    let active = null;
    let timeout = null;
    let pendingOperation = null;
    let nextOperationId = 0;
    let dirty = false;
    let canEdit = false;
    let pendingInput = false;
    let imageLoading = false;
    let downloadName = '';
    const beforeUnload = event => {
        if (dirty || pendingInput || imageLoading || pendingOperation?.type === 'edit' || active?.type === 'edit') {
            event.preventDefault();
            event.returnValue = '';
        }
    };
    const queue = new Map();
    const cancelled = new Set();

    function emit(message) {
        const bytes = new TextEncoder().encode(JSON.stringify(message) + '\0');
        const pointer = runtime._malloc(bytes.length);
        if (!pointer) throw new Error('Browser memory is full.');
        try {
            runtime.HEAPU8.set(bytes, pointer);
            runtime._trepdfWebMessage(pointer);
        } finally { runtime._free(pointer); }
    }

    function stopWorker() {
        if (timeout !== null) browser.clearTimeout(timeout);
        timeout = null;
        if (worker) worker.terminate();
        worker = null;
        active = null;
        pendingOperation = null;
        queue.clear();
        cancelled.clear();
    }

    function fail(message) {
        stopWorker();
        opened = false;
        originalFile = candidateFile = null;
        emit({type: 'error', generation, message, fatal: true});
    }

    function armTimeout() {
        if (timeout !== null) browser.clearTimeout(timeout);
        timeout = browser.setTimeout(() => fail('The PDF took too long to process. Try a smaller file.'), 30000);
    }

    function schedule() {
        if (!worker || active || !opened) return;
        if (pendingOperation) {
            active = pendingOperation;
            pendingOperation = null;
        } else {
            if (queue.size === 0) return;
            const [requestId, request] = queue.entries().next().value;
            queue.delete(requestId);
            active = request;
        }
        armTimeout();
        worker.postMessage(active, active.command?.image?.bytes instanceof ArrayBuffer
            ? [active.command.image.bytes] : []);
    }

    function submitOperation(type, command) {
        if (!opened || !canEdit || pendingOperation || (active && active.operationId)) return false;
        pendingOperation = {type, generation, operationId: ++nextOperationId, command};
        schedule();
        return true;
    }

    function download(blob, name) {
        const url = browser.URL.createObjectURL(blob);
        const link = browser.document.createElement('a');
        link.href = url;
        link.download = name;
        browser.document.body.appendChild(link);
        link.click();
        link.remove();
        browser.setTimeout(() => browser.URL.revokeObjectURL(url), 1000);
    }

    function createWorker() {
        const current = new browser.Worker(new URL('pdfium/pdfium-worker.js', browser.document.baseURI));
        worker = current;
        current.onerror = () => {
            if (worker === current) fail('The PDF engine could not run. Reload the page and try again.');
        };
        current.onmessageerror = current.onerror;
        current.onmessage = event => {
            if (worker !== current) return;
            const message = event.data;
            if (message.type === 'ready') {
                if (!candidateFile && !active) {
                    if (timeout !== null) browser.clearTimeout(timeout);
                    timeout = null;
                }
                emit({type: 'ready', generation});
                return;
            }
            if (message.type === 'error' && message.generation === -1) {
                fail(message.message);
                return;
            }
            if (message.generation !== generation) return;
            if (message.type === 'indexed') {
                if (active?.type === 'render' && active.requestId === message.requestId)
                    emit(message);
                return;
            }
            const isOperationReply = (message.type === 'edited' || message.type === 'exported'
                || message.type === 'error') && Number.isSafeInteger(message.operationId);
            if (isOperationReply) {
                if (!active || message.operationId !== active.operationId) return;
                if (timeout !== null) browser.clearTimeout(timeout);
                timeout = null;
                active = null;
                if (message.type === 'exported') {
                    if (!(message.bytes instanceof ArrayBuffer) || message.bytes.byteLength === 0
                        || message.bytes.byteLength > maximumFileBytes) {
                        emit({type: 'error', generation, operationId: message.operationId,
                            message: 'The PDF export returned invalid data. Your edits are retained.'});
                        schedule();
                        return;
                    }
                    originalFile = new browser.Blob([message.bytes], {type: 'application/pdf'});
                    downloadName = downloadName.replace(/(?:-edited)?\.pdf$/i, '') + '-edited.pdf';
                    download(originalFile, downloadName);
                }
                if (message.type !== 'error') dirty = !!message.dirty;
                // A successful edit resets all Qt page rasters. Export and failed
                // edits keep the same pages, so their outstanding renders must finish.
                if (message.type === 'edited') queue.clear();
                const {bytes, ...state} = message;
                emit(state);
                schedule();
                return;
            }
            const isRenderReply = message.type === 'rendered' || (message.type === 'error' && message.requestId);
            if (isRenderReply && (!active || message.requestId !== active.requestId)) return;
            if (message.type !== 'opened' && message.type !== 'error' && !isRenderReply) return;
            if (timeout !== null) browser.clearTimeout(timeout);
            timeout = null;
            if (message.type === 'opened') {
                opened = true;
                canEdit = !!message.canEdit;
                dirty = false;
                downloadName = candidateFile.name;
                originalFile = candidateFile;
                candidateFile = null;
                emit(message);
            } else if (isRenderReply) {
                active = null;
                if (!cancelled.delete(message.requestId)) {
                    if (message.type === 'error') {
                        emit(message);
                    } else {
                        const size = message.width * message.height * 4;
                        if (!Number.isInteger(message.width) || !Number.isInteger(message.height)
                            || message.width < 1 || message.height < 1
                            || message.width > 4096 || message.height > 4096 || size > 16000000
                            || !(message.bytes instanceof ArrayBuffer) || message.bytes.byteLength !== size) {
                            fail('The PDF engine returned an invalid page.');
                            return;
                        }
                        const pointer = runtime._malloc(size);
                        if (!pointer) { fail('Browser memory is full. Try a smaller PDF.'); return; }
                        try {
                            runtime.HEAPU8.set(new Uint8Array(message.bytes), pointer);
                            runtime._trepdfWebPixels(message.requestId, generation, message.width, message.height, pointer);
                        } finally { runtime._free(pointer); }
                    }
                }
            } else if (message.type === 'error') {
                fail(message.message);
                return;
            }
            schedule();
        };
        return current;
    }

    async function openSelectedFile(file) {
        if (!file) return;
        if (file.size === 0 || file.size > maximumFileBytes) {
            emit({type: 'error', generation, message: 'Choose a PDF between 1 byte and 64 MiB.'});
            return;
        }
        generation++;
        stopWorker();
        dirty = pendingInput = imageLoading = canEdit = false;
        originalFile = null;
        candidateFile = file;
        opened = false;
        const requestedGeneration = generation;
        emit({type: 'loading', generation, fileName: file.name});
        try {
            const bytes = await file.arrayBuffer();
            if (requestedGeneration !== generation) return;
            armTimeout();
            createWorker().postMessage({type: 'open', generation, bytes}, [bytes]);
        } catch (_) {
            if (requestedGeneration === generation) fail('This PDF could not be opened. Please choose it again.');
        }
    }

    async function replaceSelectedImage(file, id, selectedGeneration) {
        if (!file || selectedGeneration !== generation || !opened) return;
        imageLoading = true;
        emit({type: 'editing', generation});
        let bitmap;
        try {
            if (!['image/png', 'image/jpeg', 'image/webp'].includes(file.type)
                || file.size <= 0 || file.size > 16 * 1024 * 1024)
                throw new Error('Choose a PNG, JPEG or WebP image up to 16 MiB.');
            bitmap = await browser.createImageBitmap(file);
            if (selectedGeneration !== generation || !opened) return;
            const {width, height} = bitmap;
            if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1
                || width > 4096 || height > 4096 || width * height > 4000000)
                throw new Error('Choose an image up to 4 megapixels and 4096 pixels per side.');
            const canvas = browser.OffscreenCanvas ? new browser.OffscreenCanvas(width, height)
                : browser.document.createElement('canvas');
            canvas.width = width;
            canvas.height = height;
            const context = canvas.getContext('2d', {willReadFrequently: true});
            if (!context) throw new Error('This browser cannot decode the selected image.');
            context.drawImage(bitmap, 0, 0);
            const bytes = context.getImageData(0, 0, width, height).data.buffer;
            imageLoading = false;
            if (!submitOperation('edit', {action: 'replaceImage', object: {id},
                                         image: {width, height, bytes}}))
                throw new Error('Wait for the current operation, then try replacing the image again.');
        } catch (error) {
            if (selectedGeneration === generation && opened)
                emit({type: 'error', generation, message: error.message || 'The image could not be loaded.'});
        } finally {
            bitmap?.close();
            if (selectedGeneration === generation) imageLoading = false;
        }
    }

    return {
        initialize() {
            browser.addEventListener?.('beforeunload', beforeUnload);
            try { armTimeout(); createWorker(); }
            catch (_) { fail('This browser cannot start the PDF viewer.'); }
        },
        chooseFile() {
            if (picker) return;
            picker = browser.document.createElement('input');
            picker.type = 'file';
            picker.accept = 'application/pdf,.pdf';
            picker.style.display = 'none';
            picker.setAttribute('aria-label', 'Open PDF');
            const input = picker;
            const remove = () => { input.remove(); if (picker === input) picker = null; };
            input.addEventListener('cancel', remove, {once: true});
            input.addEventListener('change', () => {
                const file = input.files[0];
                remove();
                void openSelectedFile(file);
            }, {once: true});
            browser.document.body.appendChild(input);
            input.click();
        },
        replaceImage(id) {
            if (!opened || !canEdit || picker || imageLoading || pendingOperation
                || active?.operationId || !Number.isInteger(id) || id <= 0) return false;
            picker = browser.document.createElement('input');
            picker.type = 'file';
            picker.accept = 'image/png,image/jpeg,image/webp';
            picker.style.display = 'none';
            picker.setAttribute('aria-label', 'Replace PDF image');
            const input = picker;
            const selectedGeneration = generation;
            const remove = () => { input.remove(); if (picker === input) picker = null; };
            input.addEventListener('cancel', remove, {once: true});
            input.addEventListener('change', () => {
                const file = input.files[0];
                remove();
                void replaceSelectedImage(file, id, selectedGeneration);
            }, {once: true});
            browser.document.body.appendChild(input);
            input.click();
            return true;
        },
        closeFile() {
            if (picker) { picker.remove(); picker = null; }
            generation++;
            stopWorker();
            originalFile = candidateFile = null;
            opened = false;
            dirty = pendingInput = imageLoading = canEdit = false;
            emit({type: 'closed', generation});
        },
        downloadFile() {
            if (!opened || !originalFile || imageLoading || pendingOperation || (active && active.operationId)) return;
            if (dirty) {
                if (!submitOperation('export'))
                    emit({type: 'error', generation, message: 'The PDF cannot be exported right now. Try again.'});
            } else {
                download(originalFile, downloadName);
            }
        },
        edit(json) {
            if (imageLoading) return false;
            let command;
            try { command = JSON.parse(json); } catch (_) { return false; }
            return submitOperation('edit', command);
        },
        setPendingInput(value) { pendingInput = !!value; },
        render(requestId, page, width) {
            if (!opened) return;
            if (queue.size >= 64) {
                emit({type: 'error', generation, requestId, message: 'Too many pages are waiting. Scroll back to retry.'});
                return;
            }
            queue.set(requestId, {type: 'render', generation, requestId, page, width});
            schedule();
        },
        cancel(requestId) {
            queue.delete(requestId);
            if (active && active.requestId === requestId) cancelled.add(requestId);
        },
        dispose() {
            browser.removeEventListener?.('beforeunload', beforeUnload);
            dirty = pendingInput = imageLoading = canEdit = false;
            generation++;
            stopWorker();
            opened = false;
            originalFile = candidateFile = null;
            if (picker) { picker.remove(); picker = null; }
        }
    };
}

Module.trepdfWeb = createTrePDFBrowserBridge(Module, globalThis);
