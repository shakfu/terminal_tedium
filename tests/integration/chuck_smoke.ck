Tedium t;
<<< "numCV:", t.numCV() >>>;
<<< "cv(0):", t.cv(0) >>>;
<<< "trig(0):", t.trig(0) >>>;
<<< "button(0):", t.button(0) >>>;

TediumCV cvu => blackhole;
0 => cvu.select;
TediumTrig tru => blackhole;
0 => tru.select;

200::ms => now;
<<< "cv ugen:", cvu.last() >>>;
<<< "trig ugen:", tru.last() >>>;
