// Run against the packaged site, with Playwright available through NODE_PATH.
// NODE_PATH=/path/to/node_modules node scripts/ci/tests/test_web_browser.cjs build/web-site
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const http = require('node:http');
const path = require('node:path');
const { chromium } = require('playwright');
const { PNG } = require('pngjs');

const site = path.resolve(process.argv[2] || 'build/web-site');
const screenshots = process.env.TREPDF_WEB_SCREENSHOTS;

function fixture() {
    const objects = [
        '<< /Type /Catalog /Pages 2 0 R >>',
        '<< /Type /Pages /Kids [4 0 R 6 0 R 8 0 R] /Count 3 >>',
        '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>'
    ];
    ['0.85 0.25 0.2', '0.2 0.6 0.35', '0.2 0.4 0.85'].forEach((color, index) => {
        const content = `${color} rg 0 0 600 800 re f\n1 1 1 rg BT /F1 32 Tf 60 700 Td (TrePDF browser test - Page ${index + 1}) Tj ET\n`;
        objects.push(`<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /Font << /F1 3 0 R >> >> /Contents ${5 + index * 2} 0 R >>`,
            `<< /Length ${content.length} >>\nstream\n${content}endstream`);
    });
    let pdf = '%PDF-1.7\n';
    const offsets = [];
    objects.forEach((object, index) => {
        offsets.push(pdf.length);
        pdf += `${index + 1} 0 obj\n${object}\nendobj\n`;
    });
    const start = pdf.length;
    pdf += `xref\n0 ${objects.length + 1}\n0000000000 65535 f \n`;
    for (const offset of offsets) pdf += `${String(offset).padStart(10, '0')} 00000 n \n`;
    pdf += `trailer\n<< /Size ${objects.length + 1} /Root 1 0 R >>\nstartxref\n${start}\n%%EOF\n`;
    return Buffer.from(pdf);
}

// Serve only the staged website, never the checkout or SDK.
const server = http.createServer(async (request, response) => {
    try {
        const pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
        const filename = path.resolve(site, '.' + (pathname === '/' ? '/index.html' : pathname));
        if (!filename.startsWith(site + path.sep)) throw new Error('Outside site');
        const content = await fs.readFile(filename);
        const types = { '.wasm': 'application/wasm', '.js': 'text/javascript', '.html': 'text/html' };
        response.setHeader('Content-Type', types[path.extname(filename)] || 'application/octet-stream');
        response.end(content);
    } catch (_) { response.writeHead(404); response.end(); }
});

(async () => {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    let browser;
    try {
        browser = await chromium.launch({ headless: true,
            ...(process.env.TREPDF_TEST_CHROME_CHANNEL ? { channel: process.env.TREPDF_TEST_CHROME_CHANNEL } : {}) });
        const page = await browser.newPage({ viewport: { width: 1100, height: 800 }, acceptDownloads: true });
        const errors = [];
        page.on('pageerror', error => { errors.push(error.message); console.error(error.stack); });
        // Observe real Worker traffic without changing the application or render replies.
        await page.addInitScript(() => {
            window.pdfMessages = [];
            const OriginalWorker = window.Worker;
            window.Worker = class extends OriginalWorker {
                constructor(...args) {
                    super(...args);
                    this.addEventListener('message', event => {
                        const { bytes, ...message } = event.data;
                        window.pdfMessages.push(message);
                    });
                }
            };
        });
        await page.goto(`http://127.0.0.1:${server.address().port}/`);
        const waitFor = async (type, generation, targetPage) => page.waitForFunction(
            ({ type, generation, targetPage }) => window.pdfMessages.some(message =>
                message.type === type && (generation === undefined || message.generation === generation)
                && (targetPage === undefined || message.page === targetPage)),
            { type, generation, targetPage }, { timeout: 60000 });
        await waitFor('ready');
        const pdf = fixture();
        async function choose(name, buffer) {
            const chooser = page.waitForEvent('filechooser');
            // Qt Quick renders controls in a canvas; use the stable desktop toolbar layout.
            await page.mouse.click(880, 20);
            await (await chooser).setFiles({ name, mimeType: 'application/pdf', buffer });
        }
        async function screenshot(name) {
            if (!screenshots) return;
            await fs.mkdir(screenshots, { recursive: true });
            await page.screenshot({ path: path.join(screenshots, name + '.png') });
        }
        async function matchesPageColor(expected) {
            // Check the actual Qt canvas, not just a successful worker message.
            const image = PNG.sync.read(await page.screenshot());
            const offset = (400 * image.width + 500) * 4;
            const actual = [...image.data.subarray(offset, offset + 3)];
            return actual.every((value, index) => Math.abs(value - expected[index]) < 8);
        }
        async function assertPageColor(expected) {
            for (let retry = 0; retry < 30; retry++) {
                if (await matchesPageColor(expected)) return;
                await page.waitForTimeout(100);
            }
            await screenshot('unexpected-color');
            assert.fail(`visible PDF color should be ${expected}`);
        }
        await choose('browser-test.pdf', pdf);
        await waitFor('opened', 1);
        await waitFor('rendered', 1, 0);
        await page.waitForTimeout(300);
        await assertPageColor([217, 64, 51]);
        await screenshot('page-one');

        const downloadReady = page.waitForEvent('download');
        await page.mouse.click(985, 20);
        const download = await downloadReady;
        assert.equal(download.suggestedFilename(), 'browser-test.pdf');
        assert.deepEqual(await fs.readFile(await download.path()), pdf, 'download preserves every original byte');

        await page.mouse.move(700, 500);
        // Native wheel scaling differs between Linux Chromium and macOS Chrome.
        for (let step = 0; step < 8; step++) {
            await page.mouse.wheel(0, 800);
            await page.waitForTimeout(200);
            if (await matchesPageColor([51, 153, 89])) break;
        }
        await waitFor('rendered', 1, 1);
        await page.waitForTimeout(300);
        await assertPageColor([51, 153, 89]);
        await screenshot('page-two');
        // Zoom out generates a new raster size, including for the visible page.
        const beforeZoom = await page.evaluate(() => window.pdfMessages.filter(m => m.type === 'rendered').length);
        await page.mouse.click(910, 60);
        await page.waitForFunction(count => window.pdfMessages.filter(m => m.type === 'rendered').length > count,
            beforeZoom, { timeout: 15000 });
        await page.waitForTimeout(200);
        await assertPageColor([51, 153, 89]);
        await screenshot('zoom');

        await page.mouse.click(130, 70); // Next page
        await waitFor('rendered', 1, 2);
        await page.waitForTimeout(200);
        await assertPageColor([51, 102, 217]);

        await page.mouse.click(1070, 20); // Close
        await choose('invalid.pdf', Buffer.from('not a PDF'));
        await waitFor('error', 3);
        await choose('recovered.pdf', pdf);
        await waitFor('rendered', 4, 0);
        await page.setViewportSize({ width: 768, height: 1024 });
        await page.waitForTimeout(500);
        await assertPageColor([217, 64, 51]);
        await screenshot('tablet');
        assert.deepEqual(errors, [], 'no JavaScript/WebAssembly runtime errors');
        console.log('Browser: open, render, scroll, zoom, byte-exact download, close, invalid-file recovery and resize passed.');
    } finally {
        if (browser) await browser.close();
        await new Promise(resolve => server.close(resolve));
    }
})().catch(error => { console.error(error); process.exitCode = 1; });
