import json,urllib.request,time
for i in range(10):
 out={'i':i}
 for name,url in [('mode','http://10.23.38.181/api/mode'),('results','http://127.0.0.1:9000/results')]:
  t=time.monotonic()
  try:
   d=json.load(urllib.request.urlopen(url,timeout=2));out[name+'_rtt_ms']=round((time.monotonic()-t)*1000)
   if name=='mode':out['mode']={k:d.get(k) for k in ['controller_boot_id','session_id','seq','engagement_token','rate_half','pause_reason','remaining_ms','http_stack_free_bytes']}
   else:
    r=d.get('results',{}).get('focus',{}).get('result',{});out['focus']={'level':r.get('engage_detect_result',{}).get('engage_level_name'),**r.get('sampling',{})};out['dispatch']=d.get('algorithm_diagnostics',{}).get('focus')
  except Exception as e:out[name+'_error']=str(e)
 print(json.dumps(out,ensure_ascii=False),flush=True);time.sleep(.6)
