import unittest
from phone_use_policy import PhoneUsePolicy

class PresenceTests(unittest.TestCase):
    def test_one_miss_checks_two_misses_confirm_absence_without_preserving_usage(self):
        p=PhoneUsePolicy()
        def observe(t,detected):return p.observe({'phone_detected':detected,'hand_overlap':detected},t,t,t+.1)
        for t in range(100,104):value=observe(t,True)
        self.assertEqual(value['status'],'suspected')
        missed=observe(104,False)
        self.assertEqual(missed['presence_state'],'checking');self.assertFalse(missed['phone_detected'])
        self.assertEqual(missed['sustained_ms'],0)
        returned=observe(105,True)
        self.assertEqual(returned['presence_state'],'detected');self.assertEqual(returned['sustained_ms'],0)
        observe(106,False);self.assertEqual(observe(107,False)['presence_state'],'not_detected')

    def test_expired_presence_and_reset_cannot_keep_old_positive(self):
        p=PhoneUsePolicy();p.observe({'phone_detected':True,'hand_overlap':True},1,100,100.1)
        p.interrupt_overlap()
        value=p.observe({'phone_detected':False,'hand_overlap':False},2,101,101.1)
        self.assertEqual(value['presence_state'],'checking')
        p.reset()
        value=p.observe({'phone_detected':False,'hand_overlap':False},3,102,102.1)
        self.assertEqual(value['presence_state'],'not_detected')
        p.observe({'phone_detected':True,'hand_overlap':True},4,103,103.1)
        value=p.observe({'phone_detected':False,'hand_overlap':False},5,106,106.1)
        self.assertEqual(value['presence_state'],'not_detected')

if __name__=='__main__':unittest.main()
