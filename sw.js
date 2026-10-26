const V='hf28-v1',SH=['./','index.html','historico.html','gh.js','manifest.webmanifest','icon-192.png','icon-512.png'];
self.addEventListener('install',e=>e.waitUntil(caches.open(V).then(c=>c.addAll(SH)).then(()=>self.skipWaiting())));
self.addEventListener('activate',e=>e.waitUntil(caches.keys().then(k=>Promise.all(k.filter(x=>x!==V).map(x=>caches.delete(x)))).then(()=>self.clients.claim())));
const keep=(q,r)=>{const c=r.clone();caches.open(V).then(x=>x.put(q,c));return r};
self.addEventListener('fetch',e=>{
  const u=new URL(e.request.url);
  if(e.request.method!=='GET'||u.hostname==='api.github.com')return;
  if(u.hostname==='cdnjs.cloudflare.com')e.respondWith(caches.match(e.request).then(m=>m||fetch(e.request).then(r=>keep(e.request,r))));
  else e.respondWith(fetch(e.request).then(r=>keep(e.request,r)).catch(()=>caches.match(e.request)));
});
