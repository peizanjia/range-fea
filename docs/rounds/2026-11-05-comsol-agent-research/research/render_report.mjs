// Local report rendering only; no remote scripts, fonts, or dependencies are loaded.
import {readFile, writeFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {dirname, join} from 'node:path';
import {marked} from '/home/erwin/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/marked/lib/marked.esm.js';
const here=dirname(fileURLToPath(import.meta.url));
const base=process.argv[2] ?? 'COMSOL_FULL_WORKFLOW_AGENT_REPORT';
if(!/^[A-Za-z0-9_]+$/.test(base))throw new Error('Invalid report basename');
const source=await readFile(join(here,base+'.md'),'utf8');
marked.setOptions({gfm:true,breaks:false});
let body=marked.parse(source);
let count=0;
const entries=[];
body=body.replace(/<h([23])>(.*?)<\/h\1>/gs,(_,level,title)=>{
  const id=`section-${++count}`;
  if(level==='2')entries.push(`<a href="#${id}">${title}</a>`);
  return `<h${level} id="${id}">${title}</h${level}>`;
});
body=body.replace(/<p>\[(\d+)\] /g,'<p id="reference-$1">[$1] ');
body=body.replace(/\[(\d+)\]/g,'<a class="citation" href="#reference-$1">[$1]</a>');
const html=`<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>COMSOL 与 Range FEA 全流程智能体研究 · 2026-11-05</title>
<style>
:root{font-family:"Noto Sans CJK SC",sans-serif;color:#222;background:#f4f4f1;line-height:1.75}
body{margin:0}main{max-width:1100px;margin:24px auto;background:white;padding:42px 50px}
h1{font-size:28px;line-height:1.4;margin:0 0 28px}h2{font-size:22px;margin:40px 0 14px;border-bottom:1px solid #ccc;padding-bottom:8px}
h3{font-size:17px;margin:28px 0 10px}p{margin:12px 0}a{color:#155d8a;text-decoration-thickness:1px;overflow-wrap:anywhere}
table{border-collapse:collapse;width:100%;font-size:13px;line-height:1.65;margin:20px 0}
th,td{border:1px solid #ccc;padding:9px;vertical-align:top;overflow-wrap:anywhere}th{background:#eef1f3;text-align:left}
code{font-family:"Noto Sans Mono CJK SC",monospace;font-size:.88em;overflow-wrap:anywhere;background:#f0f1f2;padding:1px 3px}
pre{white-space:pre-wrap;overflow-wrap:anywhere;padding:14px;background:#f0f1f2}.citation{font-size:.85em;white-space:nowrap}
nav{border:1px solid #ccc;padding:18px 22px;background:#fafafa}nav a{display:block;font-size:14px}
@page{size:A4;margin:15mm 14mm 16mm}
@media print{ :root{background:white;font-size:10pt;line-height:1.62}main{max-width:none;margin:0;padding:0}
 h1{font-size:21pt}h2{font-size:15pt;margin-top:24pt;break-after:avoid}h3{font-size:11.5pt;break-after:avoid}
 table{font-size:8pt;line-height:1.5}th,td{padding:5pt}tr{break-inside:avoid}thead{display:table-header-group}
 p{orphans:3;widows:3}nav{display:none}a{color:#174c68;text-decoration:none}pre{font-size:8pt}}
</style><main>
<nav aria-label="报告目录">${entries.join('\n')}</nav>${body}</main></html>`;
await writeFile(join(here,base+'.html'),html);
console.log(JSON.stringify({output:join(here,base+'.html'),sections:entries.length,bytes:Buffer.byteLength(html)}));
