## Test case for Ren'Py <= 8.3 (old test language). tools/run_renpy_e2e.py copies the
## right dialect into game/ as e2e_testcase.rpy.

testcase default:
    # Synthetic clicks post move+down+up at once; moving first and pausing lets Ren'Py
    # update hover focus so the press is not dropped (a human pointer always arrives first).
    $ _test.timeout = 30.0
    $ _test.transition_timeout = 0.05
    # The only enabled network is chosen automatically; wait for the session.
    pause 0.2 until "e2e-session"
    pause 0.5
    move "Phone wallet"
    pause 0.3
    "Phone wallet"
    pause 0.5
    move "Back"
    pause 0.3
    "Back"
    pause 0.5
    move "Manual transfer"
    pause 0.3
    "Manual transfer"
    pause 0.5
    move "I've sent it"
    pause 0.3
    "I've sent it"
    pause 0.5
    assert e2e_step() == "waiting"
    $ e2e_pay_open_checkout()
    pause 0.5 until "e2e-paid"
    assert e2e_step() == "done"
    assert scfg.owns("chapter2")
    move "Continue"
    pause 0.3
    "Continue"
    # pay() returned; the script opens the restore screen next.
    pause 0.2 until "e2e-restore-open"
    assert e2e_status == "paid"
    assert len(persistent.scfg_receipts) == 1
    $ e2e_restore_ctl().tx_input = e2e_last_tx
    move "Check"
    pause 0.3
    "Check"
    pause 0.5 until "e2e-restore-done"
    $ e2e_report(status=e2e_status, owned=scfg.owns("chapter2"), receipts=len(persistent.scfg_receipts), tx=e2e_last_tx, purchases_listed=len(scfg.receipts()), restore_state=e2e_restore_ctl().state)
    move "Close"
    pause 0.3
    "Close"
    pause 2.0
