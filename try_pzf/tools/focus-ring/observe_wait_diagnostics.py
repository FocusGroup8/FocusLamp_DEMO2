import json,urllib.request,time
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
for i in range(8):
 out={'i':i}
 for name,url in [('mode','http://10.23.38.181/api/mode'),('results','http://127.0.0.1:9000/results')]:
  try:
   with opener.open(url,timeout=2) as f:d=json.load(f)
   if name=='mode':out['mode']={k:d.get(k) for k in ['session_id','engagement_token','rate_half','pause_reason','remaining_ms']}
   else:
    out['transport']=d.get('focus_transport');out['sampling']=d.get('results',{}).get('focus',{}).get('result',{}).get('sampling')
  except Exception as e:out[name+'_error']=str(e)
 print(json.dumps(out),flush=True);time.sleep(.7)
