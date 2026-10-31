// Lexer traps: nothing in strings, templates, regexes or comments is a
// dependency. The only real one is the require at the end.
/* import fake from 'comment-block'; require('comment-block') */
// require('comment-line')
const notImport = "import x from 'string-double'";
const alsoNot = 'require("string-single")';
const tpl = `require('template') ${ `nested ${'require("nested-string")'}` } import('template-tail')`;
const re = /require\('regex'\)[/]import/g;
const ratio = 10 / 2 / 1;          // divisions, not regexes
const re2 = ratio > 1 ? /'/ : /"/;  // regexes holding quotes
const obj = { require: 1, import: 2 };
const member = obj.require + obj.import;
function f() { return /\/import('x')/.test('y'); }
const real = require('./store.cjs');
module.exports = { notImport, alsoNot, tpl, re, re2, member, f, real };
