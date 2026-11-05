// Deterministic PRNG and word source shared by the workload pages.
var seed = 12345;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; }
var WORDS = 'lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua enim ad minim veniam quis nostrud exercitation ullamco laboris nisi aliquip ex ea commodo consequat duis aute irure in reprehenderit voluptate velit esse cillum fugiat nulla pariatur excepteur sint occaecat cupidatat non proident sunt culpa qui officia deserunt mollit anim id est laborum'.split(' ');
function sentence(n) { var s = []; for (var i = 0; i < n; i++) s.push(WORDS[(rnd() * WORDS.length) | 0]); var t = s.join(' '); return t[0].toUpperCase() + t.slice(1) + '.'; }
