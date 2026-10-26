/* Banco SQL (SQLite) guardado num repositório do GitHub. Usado por index.html e historico.html */
const CDN='https://cdnjs.cloudflare.com/ajax/libs/sql.js/1.8.0/';
const b64=u=>{let s='';for(let i=0;i<u.length;i+=0x8000)s+=String.fromCharCode.apply(null,u.subarray(i,i+0x8000));return btoa(s)};
const GH={
  DB:'data/abastecimentos.db',
  SCHEMA:'CREATE TABLE IF NOT EXISTS abastecimentos(uid TEXT PRIMARY KEY,inicio TEXT,fim TEXT,duracao_s INTEGER,colaborador TEXT,cracha_id TEXT,maquina TEXT,totem_id TEXT,produto TEXT,litros REAL,obs TEXT,foto TEXT)',
  cfg(){try{return JSON.parse(localStorage.getItem('gh')||'{}')}catch(e){return{}}},
  set(c){localStorage.setItem('gh',JSON.stringify(c))},
  ok(){const c=this.cfg();return !!(c.repo&&c.token)},
  h(raw){return{Authorization:'Bearer '+this.cfg().token,Accept:raw?'application/vnd.github.raw+json':'application/vnd.github+json'}},
  u(p,get){const c=this.cfg();return 'https://api.github.com/repos/'+c.repo+'/contents/'+p+(get&&c.branch?'?ref='+encodeURIComponent(c.branch):'')},
  async test(){
    const r=await fetch('https://api.github.com/repos/'+this.cfg().repo,{headers:this.h()});
    if(!r.ok)throw new Error(r.status===404?'repositório não encontrado ou token sem acesso':r.status===401?'token inválido':'GitHub '+r.status);
    return (await r.json()).private;
  },
  _sql:null,
  async sql(){
    if(this._sql)return this._sql;
    if(!window.initSqlJs)await new Promise((ok,no)=>{const s=document.createElement('script');s.src=CDN+'sql-wasm.js';s.onload=ok;s.onerror=()=>no(new Error('sem acesso ao SQL.js (precisa de internet na primeira vez)'));document.head.appendChild(s)});
    return this._sql=await initSqlJs({locateFile:f=>CDN+f});
  },
  async read(){
    const m=await fetch(this.u(this.DB,1),{headers:this.h(),cache:'no-store'});
    if(m.status===404)return null;
    if(!m.ok)throw new Error(m.status===401?'token inválido':'GitHub '+m.status);
    const sha=(await m.json()).sha;
    const r=await fetch(this.u(this.DB,1),{headers:this.h(1),cache:'no-store'});
    if(!r.ok)throw new Error('GitHub '+r.status);
    return{sha,bytes:new Uint8Array(await r.arrayBuffer())};
  },
  put(path,b64s,msg,sha){
    return fetch(this.u(path),{method:'PUT',headers:this.h(),body:JSON.stringify({message:msg,content:b64s,sha:sha||undefined,branch:this.cfg().branch||undefined})});
  },
  async push(rows,save){
    try{await this._push(rows,save)}catch(e){throw new Error((e&&e.message)||String(e))}
  },
  async _push(rows,save){
    if(!this.ok())throw new Error('configure o GitHub em Ajustes');
    for(const r of rows)if(!r.uid){r.uid=crypto.randomUUID();await save(r)}
    for(const r of rows){
      if(r.foto&&!r.fotoUp){
        const x=await this.put('fotos/'+r.uid+'.jpg',r.foto.split(',')[1],'foto '+r.uid);
        if(!x.ok&&x.status!==422)throw new Error('foto: GitHub '+x.status);
        r.fotoUp=true;await save(r);
      }
    }
    const S=await this.sql();
    for(let t=0;t<4;t++){
      const cur=await this.read();
      const d=cur&&cur.bytes.length?new S.Database(cur.bytes):new S.Database();
      d.run(this.SCHEMA);
      for(const r of rows)d.run('INSERT OR IGNORE INTO abastecimentos VALUES (?,?,?,?,?,?,?,?,?,?,?,?)',[r.uid,r.inicio,r.fim,r.duracao_s,r.colaborador,r.cracha_id,r.maquina,r.totem_id,r.produto,r.litros,r.obs,r.foto?'fotos/'+r.uid+'.jpg':''].map(v=>v===undefined?null:v));
      const out=d.export();d.close();
      const x=await this.put(this.DB,b64(out),'abastecimentos: +'+rows.length,cur&&cur.sha);
      if(x.ok){for(const r of rows){r.sincronizado=true;await save(r)}return}
      if(x.status!==409&&x.status!==422)throw new Error(x.status===401||x.status===403?'token sem permissão de escrita':'GitHub '+x.status);
    }
    throw new Error('conflito ao gravar, tente de novo');
  }
};
