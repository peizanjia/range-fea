"""Protocol regression against the real native external controller process."""
import copy
import json
import subprocess
import sys
import unittest

PROGRAM = sys.argv.pop(1)


def handshake():
    return {'protocolVersion':1, 'op':'handshake', 'requestId':'hello',
            'controllerId':'cpp', 'controllerRevision':'a'*64, 'modelSnapshotSha256':'b'*64,
            'deterministicSeed':'7', 'capabilities':{'pureStep':True,
              'controlMode':'scalar-nodal-force','sensors':[{'sensorId':'v','unit':'m/s'},
               {'sensorId':'q','unit':'m'}], 'actuatorIds':['force']}}


def compute(state=None):
    return {'protocolVersion':1,'op':'compute','requestId':'compute','stepId':'1','dt':.1,
            'observations':[{'sensorId':'q','unit':'m','value':.01},
                            {'sensorId':'v','unit':'m/s','value':.25}],
            'acceptedControllerState':{} if state is None else state}


class NativeControllerTests(unittest.TestCase):
    def run_controller(self, messages, gain='2'):
        return subprocess.run([PROGRAM, gain], input=''.join(json.dumps(x)+'\n' for x in messages),
                              capture_output=True, text=True, timeout=10)

    def test_replay_restart_and_accepted_state(self):
        result = self.run_controller([handshake(), compute(), compute()])
        self.assertEqual(result.returncode,0,result.stderr)
        replies = [json.loads(x) for x in result.stdout.splitlines()]
        self.assertEqual(replies[1],replies[2])
        self.assertEqual(replies[1]['commands'],[{'actuatorId':'force','force':-.5,'unit':'N'}])
        restarted = self.run_controller([handshake(), compute(replies[1]['nextControllerState'])])
        self.assertEqual(restarted.returncode,0,restarted.stderr)
        self.assertEqual(json.loads(restarted.stdout.splitlines()[1])['nextControllerState'],{'sampleCount':2})

    def test_reject_protocol_catalog_and_numeric_faults(self):
        cases = []
        item=compute(); item['observations'][0]['sensorId']='v'; cases.append([handshake(),item])
        item=compute(); item['observations'][1]['unit']='m'; cases.append([handshake(),item])
        item=compute(); item['observations'][1]['value']=True; cases.append([handshake(),item])
        item=compute(); item['observations'][1]['value']=float('nan'); cases.append([handshake(),item])
        item=compute({'sampleCount':9007199254740991}); cases.append([handshake(),item])
        item=compute({'sampleCount':-.5}); cases.append([handshake(),item])
        item=handshake(); item['controllerRevision']='bad'; cases.append([item])
        item=handshake(); item['capabilities']['sensors'].append(copy.deepcopy(item['capabilities']['sensors'][0])); cases.append([item])
        cases.extend([[compute()], [handshake(),handshake()]])
        for index, messages in enumerate(cases):
            with self.subTest(index=index):
                result=self.run_controller(messages)
                self.assertEqual(result.returncode,2,result.stdout)
                self.assertIn('rejected request',result.stderr)

    def test_invalid_gain_and_oversize(self):
        for gain in ['-1','nan','inf','bad']:
            self.assertEqual(self.run_controller([],gain).returncode,2)
        result=subprocess.run([PROGRAM,'2'],input=' '*1048577+'\n',capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,2)
        self.assertEqual(result.stdout,'')


if __name__=='__main__': unittest.main()
