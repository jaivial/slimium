#!/usr/bin/env bash
# Compare independent CLI, legacy batch, act and flushed NDJSON pipe.
# Baseline is the pre-track modular core; override with SLIM_BASE_REV or SLIM_BASE_BIN.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
N=${SLIM_INTERACT_N:-100}
REPS=${SLIM_BENCH_REPS:-3}
BASE_REV=${SLIM_BASE_REV:-62b1866}
TMP=$(mktemp -d /tmp/slim-interact-bench.XXXXXX)
STATE=$TMP/state; mkdir -m 700 "$STATE"
HPID=
cleanup() {
    [ -n "$HPID" ] && kill "$HPID" 2>/dev/null
    SLIM_STATE_DIR="$STATE" "$ROOT/slimium" --session ib-new stop >/dev/null 2>&1 || true
    if [ -x "$TMP/base/slimium" ]; then SLIM_STATE_DIR="$STATE" "$TMP/base/slimium" --session ib-base stop >/dev/null 2>&1 || true; fi
    rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
if [ -n "${SLIM_BASE_BIN:-}" ]; then
    mkdir "$TMP/base"
    ln -s "$(realpath "$SLIM_BASE_BIN")" "$TMP/base/slimium"
else
    mkdir "$TMP/base"
    git archive "$BASE_REV" | tar -x -C "$TMP/base"
    make -C "$TMP/base" -j2 >/dev/null
fi
python3 -m http.server --bind 127.0.0.1 9488 --directory tests/fixtures >"$TMP/http.log" 2>&1 &
HPID=$!
export SLIM_STATE_DIR="$STATE"
"$TMP/base/slimium" --session ib-base start --port 9471 --user-data-dir "$TMP/udd-base" --no-gov --window 1000x700 >/dev/null
"$ROOT/slimium" --session ib-new start --port 9470 --user-data-dir "$TMP/udd-new" --no-gov --window 1000x700 >/dev/null
"$TMP/base/slimium" --session ib-base nav http://127.0.0.1:9488/interact.html >/dev/null
"$ROOT/slimium" --session ib-new nav http://127.0.0.1:9488/interact.html >/dev/null
export ROOT N REPS
python3 - <<'PY'
import json, os, shlex, statistics, subprocess, time
root=os.environ['ROOT']
# The state directory is <tmp>/state, so its sibling is the built baseline.
old=os.path.dirname(os.environ['SLIM_STATE_DIR'])+'/base/slimium'
new=root+'/slimium'; n=int(os.environ['N']); reps=int(os.environ['REPS'])

def common():
    a=[]
    for i in range(n):
        k=i%3
        if k==0:a.append(('click',['#save']))
        elif k==1:a.append(('fill',['#name','v%d'%i]))
        else:a.append(('eval',['window.clicks||0']))
    return a

def full():
    a=[]
    for i in range(n):
        k=i%5
        if k==0:a.append(('snapshot',['--interactive','--max','12']))
        elif k==1:a.append(('find',['role=button[name="Save"]']))
        elif k==2:a.append(('click',['#save']))
        elif k==3:a.append(('fill',['#name','v%d'%i]))
        else:a.append(('eval',['document.querySelector("#result").textContent']))
    return a

def invoke(binary,session,ops,mode):
    if mode=='separate':
        for cmd,args in ops:
            p=subprocess.run([binary,'--session',session,'--json',cmd,*args],capture_output=True,text=True)
            if p.returncode or not json.loads(p.stdout).get('ok'):
                raise RuntimeError('%s %s: %s %s'%(mode,cmd,p.stdout,p.stderr))
        return
    if mode=='batch':
        data='\n'.join(shlex.join([cmd,*args]) for cmd,args in ops)+'\n'
        p=subprocess.run([binary,'--session',session,'batch'],input=data,capture_output=True,text=True)
        rows=[json.loads(x) for x in p.stdout.splitlines() if x.strip()]
        if p.returncode or len(rows)!=len(ops) or not all(x.get('ok') for x in rows):
            raise RuntimeError('batch: %s %s'%(p.stdout[-500:],p.stderr))
        return
    if mode=='act':
        payload=json.dumps([{'action':cmd,'args':args} for cmd,args in ops],separators=(',',':'))
        path=os.environ['SLIM_STATE_DIR']+'/act.json'
        with open(path,'w') as f:f.write(payload)
        p=subprocess.run([binary,'--session',session,'--json','act','@'+path],capture_output=True,text=True)
        d=json.loads(p.stdout)
        if p.returncode or d.get('failures') or len(d.get('results',[]))!=len(ops):
            raise RuntimeError('act: %s %s'%(p.stdout[-500:],p.stderr))
        return
    if mode=='pipe':
        p=subprocess.Popen([binary,'--session',session,'pipe'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
        for i,(cmd,args) in enumerate(ops):
            p.stdin.write(json.dumps({'id':i,'cmd':cmd,'args':args},separators=(',',':'))+'\n');p.stdin.flush()
            line=p.stdout.readline()
            if not line:raise RuntimeError('pipe closed: '+p.stderr.read())
            d=json.loads(line)
            if not d.get('ok') or d.get('id')!=i:raise RuntimeError('pipe %d: %s'%(i,d))
        p.stdin.close()
        if p.wait(timeout=10):raise RuntimeError('pipe exit: '+p.stderr.read())
        return
    raise ValueError(mode)

def clicks(binary,session):
    p=subprocess.run([binary,'--session',session,'--json','eval','window.clicks||0'],capture_output=True,text=True,check=True)
    return json.loads(p.stdout)['value']

def measure(binary,session,ops,mode):
    rates=[]; speeds=[]
    expected_clicks=sum(cmd=='click' for cmd,args in ops)
    for _ in range(reps):
        before=clicks(binary,session)
        t=time.perf_counter();invoke(binary,session,ops,mode);dt=time.perf_counter()-t
        after=clicks(binary,session)
        if after-before!=expected_clicks:raise RuntimeError('click correctness: %s != %s'%(after-before,expected_clicks))
        rates.append(dt*1000/len(ops));speeds.append(len(ops)/dt)
    p=subprocess.run([binary,'--session',session,'--json','eval','({name:document.querySelector("#name").value,clicks:window.clicks||0})'],capture_output=True,text=True,check=True)
    d=json.loads(p.stdout)['value']
    if not d['name'].startswith('v') or d['clicks']<1:raise RuntimeError('correctness: '+str(d))
    return statistics.median(rates),statistics.median(speeds)

results={}
for label,ops,modes in [('common',common(),['separate','batch']),('common',common(),['separate','batch','act','pipe']),('full',full(),['separate','batch','act','pipe'])]:
    binary,session=(old,'ib-base') if label=='common' and modes==['separate','batch'] else (new,'ib-new')
    tag='baseline' if binary==old else 'current'
    print('WORKLOAD %s %s ops=%d reps=%d'%(label,tag,n,reps))
    for mode in modes:
        ms,speed=measure(binary,session,ops,mode)
        results[(label,tag,mode)]=(ms,speed)
        print('  %-8s median_ms/op %8.3f throughput_ops/s %9.1f correctness ok'%(mode,ms,speed))
for mode in ('act','pipe'):
    b=results[('common','current','batch')][0];x=results[('common','current',mode)][0]
    print('IMPROVEMENT %s_vs_current_batch %.1f%%'%(mode,(b-x)*100/b))
    b=results[('common','baseline','batch')][0]
    print('IMPROVEMENT %s_vs_baseline_batch %.1f%%'%(mode,(b-x)*100/b))
print('BENCHMARK correctness: all responses and final page state validated')
PY
