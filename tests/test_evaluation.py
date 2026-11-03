import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('evaluation',Path(__file__).resolve().parents[1]/'scripts/evaluate_postures.py')
eval=importlib.util.module_from_spec(spec);spec.loader.exec_module(eval)

def row(t,state,valid=True):
 return {'t':t,'data':{'posture_state':state,'metrics_valid':valid,'active_rules':['head_down'] if state=='HEAD_DOWN' else [],'latency_ms':17}}

def trial(label,states,accepted=True):
 return {'label':label,'round':1,'cue_at':0,'confirmed_at':0,'human_confirmed':accepted,
         'transition':[],'samples':[row(1+i*.25,state,state!='UNKNOWN') for i,state in enumerate(states)]}

class EvaluationTests(unittest.TestCase):
 def test_unknown_is_not_excluded(self):
  t=trial('normal',['NORMAL','UNKNOWN','HEAD_DOWN','NORMAL'])
  result=eval.summarize_trial(t)
  self.assertEqual(result['scored_status_frames'],4)
  self.assertEqual(result['primary_hit_rate'],.5)
  self.assertEqual(result['valid_geometry_rate'],.75)
 def test_independent_labels_and_precision(self):
  results=eval.summarize_session([trial('normal',['NORMAL','HEAD_DOWN']),trial('head_down',['HEAD_DOWN','UNKNOWN']),trial('head_down',['HEAD_DOWN'],False)])
  self.assertEqual(results['accepted_trials'],2)
  self.assertEqual(results['by_pose']['head_down']['precision'],.5)
  self.assertEqual(results['by_pose']['head_down']['recall'],.5)
 def test_response_requires_stable_match_after_origin(self):
  rows=[row(-1,'NORMAL'),row(-.5,'NORMAL'),row(0,'NORMAL'),row(.25,'UNKNOWN'),row(.5,'NORMAL'),row(1,'NORMAL')]
  self.assertEqual(eval.first_stable_match(rows,'normal',0),1)
 def test_no_match_is_missing_not_zero(self):
  self.assertIsNone(eval.summarize_trial(trial('head_down',['NORMAL','UNKNOWN']))['confirmation_to_stable_match_s'])

if __name__=='__main__':unittest.main()
