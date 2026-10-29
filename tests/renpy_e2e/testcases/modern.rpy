## Test case for Ren'Py >= 8.4 (new test language: click/move keywords, eval conditions).
## tools/run_renpy_e2e.py copies the right dialect into game/ as e2e_testcase.rpy.

testcase default:
    # The only enabled network is chosen automatically, so the sheet opens on the method step.
    pause 0.2 until eval e2e_has("e2e-session") timeout 60
    pause 0.2 until eval (e2e_step() == "method") timeout 20
    # A synthetic click can be dropped while Ren'Py updates focus; retry until its effect shows.
    click "Phone wallet" until eval (e2e_step() == "qr") timeout 20
    click "Back" until eval (e2e_step() == "method") timeout 20
    click "Manual transfer" until eval (e2e_step() == "manual") timeout 20
    click "I've sent it" until eval (e2e_step() == "waiting") timeout 20
    $ e2e_pay_open_checkout()
    pause 0.5 until eval e2e_has("e2e-paid") timeout 60
    assert eval (e2e_step() == "done")
    assert eval cryptopay.owns("chapter2")
    click "Continue" until eval e2e_has("e2e-restore-open") timeout 30
    assert eval (e2e_status == "paid")
    assert eval (len(persistent.cryptopay_receipts) == 1)
    $ e2e_restore_ctl().tx_input = e2e_last_tx
    click "Check" until eval (e2e_restore_ctl().state != "input") timeout 30
    pause 0.5 until eval e2e_has("e2e-restore-done") timeout 60
    $ e2e_owned_before_clear = cryptopay.owns("chapter2")
    $ e2e_receipts_before_clear = len(persistent.cryptopay_receipts)
    $ cryptopay.clear_local_data()
    pause 0.2 until eval (not cryptopay.owns("chapter2")) timeout 30
    pause 1.0
    assert eval (len(persistent.cryptopay_receipts) == 0)
    $ e2e_report(status=e2e_status, owned=e2e_owned_before_clear, receipts=e2e_receipts_before_clear, tx=e2e_last_tx, purchases_listed=1, restore_state=e2e_restore_ctl().state, cleared=not cryptopay.owns("chapter2"))
    move "Close"
    pause 0.3
    click "Close"
    pause 2.0
