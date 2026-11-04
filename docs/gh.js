/* Banco SQL (SQLite) guardado num repositório do GitHub. Usado por index.html e historico.html */
const CDN='https://cdnjs.cloudflare.com/ajax/libs/sql.js/1.8.0/';
const b64=u=>{let s='';for(let i=0;i<u.length;i+=0x8000)s+=String.fromCharCode.apply(null,u.subarray(i,i+0x8000));return btoa(s)};
const GH={
  DB:'data/abastecimentos.db',
  AUTH:{u:'12345678',p:'12345678'},
  SCHEMA:'CREATE TABLE IF NOT EXISTS abastecimentos(uid TEXT PRIMARY KEY,inicio TEXT,fim TEXT,duracao_s INTEGER,colaborador TEXT,cracha_id TEXT,maquina TEXT,totem_id TEXT,produto TEXT,litros REAL,obs TEXT,foto TEXT,oleo TEXT,preco REAL);CREATE TABLE IF NOT EXISTS oleos(uid TEXT PRIMARY KEY,nome TEXT,viscosidade TEXT,codigo TEXT,preco REAL,fornecedor TEXT,bombona REAL,obs TEXT,ativo INTEGER);CREATE TABLE IF NOT EXISTS pessoas(uid TEXT PRIMARY KEY,nome TEXT,nfc TEXT UNIQUE);CREATE TABLE IF NOT EXISTS maquinas(uid TEXT PRIMARY KEY,nome TEXT,nfc TEXT UNIQUE)',
  cfg(){try{return JSON.parse(localStorage.getItem('gh')||'{}')}catch(e){return{}}},
  set(c){localStorage.setItem('gh',JSON.stringify(c))},
  ok(){const c=this.cfg();return !!(c.repo&&c.token)},
  h(raw){return{Authorization:'Bearer '+this.cfg().token,Accept:raw?'application/vnd.github.raw+json':'application/vnd.github+json'}},
  u(p,get){const c=this.cfg();return 'https://api.github.com/repos/'+c.repo+'/contents/'+p+(get&&c.branch?'?ref='+encodeURIComponent(c.branch):'')},
  
async test(){
  const r = await fetch(
    'https://api.github.com/repos/' + this.cfg().repo,
    { headers: this.h(), cache: 'no-store' }
  );

  if(!r.ok){
    throw new Error(
      r.status === 404
        ? 'Repositório não encontrado ou token sem acesso.'
        : r.status === 401
        ? 'Token inválido ou revogado.'
        : 'GitHub HTTP ' + r.status
    );
  }

  return (await r.json()).private;
},

// Informa se o repositório foi identificado como público.
publico(){
  return this.cfg().repoPrivate === false;
},

// Testa o acesso e registra a visibilidade do repositório.
async checkPrivate(){
  const privado = await this.test();

  const atual = this.cfg();

  this.set({
    ...atual,
    repoPrivate: privado
  });

  return privado;
},

// Seleção de óleo local: usada pela tela inicial e pelo histórico.
oleoLocal(){
  try {
    return JSON.parse(localStorage.getItem('oleo') || 'null');
  } catch(e) {
    return null;
  }
},

// UID do óleo cuja troca ainda aguarda sincronização.
oleoPend(){
  return localStorage.getItem('oleoPend') || '';
},

// Confirmação visual temporária após sincronizar uma troca de óleo.
oleoRecente(){
  const raw = localStorage.getItem('oleoOk');
  if (!raw) return false;
  try {
    const info = JSON.parse(raw);
    return !!(info && Number.isFinite(info.at) && Date.now() - info.at < 10000);
  } catch(e) {
    return raw === '1';
  }
},

// Envia ao banco a seleção de óleo feita na tela inicial.
// A atualização é idempotente para permitir nova tentativa após conflito HTTP 409.
async flushOleo(){
  const uid = this.oleoPend();
  if (!uid || !this.ok()) return false;

  await this.edit(d => {
    const st = d.prepare('SELECT uid FROM oleos WHERE uid = ?');
    st.bind([uid]);
    const exists = st.step();
    st.free();

    if (!exists) {
      throw new Error('O óleo selecionado não foi encontrado no banco. Atualize os cadastros antes de tentar novamente.');
    }

    d.run('UPDATE oleos SET ativo = 0');
    d.run('UPDATE oleos SET ativo = 1 WHERE uid = ?', [uid]);
  }, 'óleo: atualizar óleo em uso');

  // Não apaga uma seleção mais recente feita enquanto a gravação estava ocorrendo.
  if (this.oleoPend() === uid) {
    localStorage.removeItem('oleoPend');
    localStorage.setItem('oleoOk', JSON.stringify({uid, at:Date.now()}));
  }
  return true;
},

// Aceita apenas caminhos de fotos gerados dentro da pasta fotos/.
fotoOk(path){
  return typeof path === 'string'
    && /^fotos\/[A-Za-z0-9][A-Za-z0-9._-]*\.jpg$/i.test(path)
    && !path.includes('..');
},

// Lista somente arquivos JPG válidos da pasta de fotos do repositório.
async listFotos(){
  if (!this.ok()) throw new Error('Configure o repositório e o token do GitHub nos Ajustes.');
  const r = await fetch(this.u('fotos', 1), {
    headers: this.h(),
    cache: 'no-store'
  });

  if (r.status === 404) return [];
  if (!r.ok) throw new Error('Não foi possível listar as fotos no GitHub (HTTP ' + r.status + ').');

  const items = await r.json();
  if (!Array.isArray(items)) return [];

  return items
    .filter(f => f && f.type === 'file' && this.fotoOk(f.path) && typeof f.sha === 'string')
    .map(f => ({path:f.path, sha:f.sha, name:f.name, size:f.size}));
},

// Exclui uma foto usando o SHA validado retornado pela API do GitHub.
async delSha(path, sha){
  if (!this.fotoOk(path)) throw new Error('Caminho de foto inválido.');
  if (typeof sha !== 'string' || !sha) throw new Error('SHA do arquivo ausente.');

  const r = await fetch(this.u(path), {
    method: 'DELETE',
    headers: this.h(),
    body: JSON.stringify({
      message: 'remover foto ' + path,
      sha,
      branch: this.cfg().branch || undefined
    })
  });

  if (r.status === 404) return false;
  if (!r.ok) throw new Error('Não foi possível excluir a foto no GitHub (HTTP ' + r.status + ').');
  return true;
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
  prep(d){
    d.run(this.SCHEMA);
    for(const c of ['oleo TEXT','preco REAL']){try{d.run('ALTER TABLE abastecimentos ADD COLUMN '+c)}catch(e){}}
  },
  async cad(){
    const cur=await this.read();if(!cur)return null;
    const S=await this.sql(),d=new S.Database(cur.bytes);this.prep(d);
    const q=t=>{const st=d.prepare('SELECT * FROM '+t+' ORDER BY nome'),o=[];while(st.step())o.push(st.getAsObject());st.free();return o};
    const r={pessoas:q('pessoas'),maquinas:q('maquinas'),oleos:q('oleos')};d.close();return r;
  },
  async edit(fn,msg){
    try{
      const S=await this.sql();
      for(let t=0;t<4;t++){
        const cur=await this.read(),d=cur&&cur.bytes.length?new S.Database(cur.bytes):new S.Database();
        this.prep(d);await fn(d);
        const out=d.export();d.close();
        const x=await this.put(this.DB,b64(out),msg,cur&&cur.sha);
        if(x.ok)return;
        if(x.status!==409&&x.status!==422)throw new Error(x.status===401||x.status===403?'token sem permissão de escrita':'GitHub '+x.status);
      }
      throw new Error('conflito ao gravar, tente de novo');
    }catch(e){throw new Error((e&&e.message)||String(e))}
  },
  async del(path){
    if (!this.fotoOk(path)) throw new Error('Caminho de foto inválido.');
    const m = await fetch(this.u(path, 1), {
      headers: this.h(),
      cache: 'no-store'
    });
    if (m.status === 404) return false;
    if (!m.ok) throw new Error('Não foi possível localizar a foto no GitHub (HTTP ' + m.status + ').');
    const item = await m.json();
    return this.delSha(path, item.sha);
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
      this.prep(d);
      for(const r of rows)d.run('INSERT OR IGNORE INTO abastecimentos(uid,inicio,fim,duracao_s,colaborador,cracha_id,maquina,totem_id,produto,litros,obs,foto,oleo,preco) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)',[r.uid,r.inicio,r.fim,r.duracao_s,r.colaborador,r.cracha_id,r.maquina,r.totem_id,r.produto,r.litros,r.obs,r.foto?'fotos/'+r.uid+'.jpg':'',r.oleo,r.preco].map(v=>v===undefined?null:v));
      const out=d.export();d.close();
      const x=await this.put(this.DB,b64(out),'abastecimentos: +'+rows.length,cur&&cur.sha);
      if(x.ok){for(const r of rows){r.sincronizado=true;await save(r)}return}
      if(x.status!==409&&x.status!==422)throw new Error(x.status===401||x.status===403?'token sem permissão de escrita':'GitHub '+x.status);
    }
    throw new Error('conflito ao gravar, tente de novo');
  }
};
