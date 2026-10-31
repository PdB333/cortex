(()=>{function n(e,t){return{text:`Hello from the ${e} theme`,level:t}}function o(e,t,r){return e<t?t:e>r?r:e}async function i(){let e=await window.cortexDemo.getSetting("theme"),t=n(String(e),o(42,0,10));return window.cortexDemo.log(t.text),t}i();})();
//# sourceMappingURL=bundle.js.map
