"""Profile construction must never mix sessions or call unsupported poses normal."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('teach', Path(__file__).resolve().parents[1] / 'scripts/teach_posture.py')
teach = importlib.util.module_from_spec(spec)
spec.loader.exec_module(teach)
BASE = dict(valid=True, created_at='2026-11-03T14:00:00', camera_id='cam',
            model_sha256='model', frame_width=640, frame_height=480)

class TeachGuardTests(unittest.TestCase):
    def fixture(self, root, other=None):
        for label, head in [('normal', 0), ('head_down', .2)]:
            data = dict(baseline=other if label == 'head_down' and other else BASE,
                        features=[[head, 0, 0, 1, 0, 3, 3] for _ in range(20)])
            (root / (label + '.json')).write_text(json.dumps(data))

    def test_matching_build_has_binding(self):
        with tempfile.TemporaryDirectory() as d, patch.object(teach, 'get_baseline', return_value=BASE):
            root=Path(d); self.fixture(root)
            out=root/'profile.txt'
            self.assertEqual(teach.build(d, str(out)), 0)
            self.assertIn('baseline_id\t'+teach.baseline_id(BASE), out.read_text())

    def test_mixed_build_never_overwrites_live_profile(self):
        with tempfile.TemporaryDirectory() as d, patch.object(teach, 'get_baseline', return_value=BASE):
            root=Path(d);self.fixture(root, dict(BASE, created_at='old'))
            out=root/'profile.txt';out.write_text('original')
            self.assertNotEqual(teach.build(d,str(out)), 0)
            self.assertEqual(out.read_text(),'original')

    def test_rebaseline_cannot_relabel_old_capture(self):
        with tempfile.TemporaryDirectory() as d, patch.object(teach, 'get_baseline', return_value=BASE):
            root=Path(d);self.fixture(root,dict(BASE,created_at='old'))
            before=(root/'head_down.json').read_bytes()
            self.assertNotEqual(teach.rebaseline(d),0)
            self.assertEqual((root/'head_down.json').read_bytes(),before)

    def test_unknown_is_not_normal_and_zero_mad_uses_floor(self):
        samples={'normal':[[0,0,0,1,0,3,3]]*10}
        self.assertEqual(teach.classify([.05,0,0,1,0,3,3],[],samples)[0],'normal')
        self.assertIsNone(teach.classify([-.3,0,0,1,0,3,3],[],samples)[0])
        self.assertIsNone(teach.classify([0,0,0,1.4,0,3,3],[],samples)[0])
        self.assertEqual(teach.classify([0,0,0,.7,0,3,3],[],samples)[0],'normal')
        self.assertEqual(teach.classify([0,0,0,1,.6,3,3],[],samples,'hand_face')[0],'normal')

    def test_missing_live_wrist_and_face_geometry_use_same_sentinels_as_cpp(self):
        b=dict(BASE,head_offset_y=-.5,width_ratio=.4)
        f=dict(valid=True,head_offset_y=-.5,shoulder_angle_deg=0,width_ratio=.4,
               wrist_valid=False,wrist_to_nose_raw=.2,face_geom_valid=False,face_wrist_ear=.1)
        self.assertEqual(teach.features_of(f,b),[0,0,0,1,0,3,3])
        f['face_geom_valid']=True
        self.assertEqual(teach.features_of(f,b)[6],.1)

    def test_missing_pose_rule_cannot_silently_publish(self):
        with tempfile.TemporaryDirectory() as d, patch.object(teach,'get_baseline',return_value=BASE):
            root=Path(d);self.fixture(root)
            path=root/'head_down.json';data=json.loads(path.read_text())
            data['features']=[[0,0,0,1,0,3,3]]*20;path.write_text(json.dumps(data))
            out=root/'profile.txt';out.write_text('original')
            self.assertNotEqual(teach.build(d,str(out)),0)
            self.assertEqual(out.read_text(),'original')

    def test_rule_build_is_independent_of_sample_file_order(self):
        samples={'normal':[[0,0,0,1,0,3,3]]*20,
                 'head_down':[[.2,4,0,1,0,3,3]]*20,
                 'head_tilt':[[.12,20,0,1,0,3,3]]*20}
        self.assertEqual(teach.build_rules(samples),teach.build_rules(dict(reversed(list(samples.items())))))

    def test_missing_distances_do_not_distort_normal_statistics(self):
        fs=[[0,0,0,1,0,3,3]]*20+[[0,0,0,1,0,1.2,1.4]]*10
        self.assertEqual(teach.feature_stat(fs,6),(1.4,0))

    def test_contact_evidence_precedes_correlated_tilt(self):
        samples={'normal':[[0,0,0,1,0,3,3]]*20,
                 'head_tilt':[[.2,20,0,1,.2,1,1]]*20,
                 'hand_face':[[.1,18,0,1,.6,.6,.6]]*20,
                 'too_close':[[.1,0,0,1.5,.7,.5,.5]]*20}
        rules=teach.build_rules(samples)
        self.assertEqual(teach.classify(samples['hand_face'][0],rules,samples)[0],'hand_face')
        self.assertEqual(teach.classify(samples['too_close'][0],rules,samples)[0],'too_close')
        self.assertEqual(teach.classify(samples['head_tilt'][0],rules,samples)[0],'head_tilt')

    def test_calibration_changed_during_capture_is_discarded(self):
        with tempfile.TemporaryDirectory() as d, \
                patch.object(teach,'get_baseline',side_effect=[BASE,dict(BASE,created_at='new')]), \
                patch.object(teach,'read_frames',return_value=[]),patch.object(teach.time,'sleep'):
            self.assertNotEqual(teach.collect('normal',3,d,0),0)
            self.assertFalse((Path(d)/'normal.json').exists())

if __name__=='__main__':
    with contextlib.redirect_stdout(io.StringIO()):
        unittest.main()
