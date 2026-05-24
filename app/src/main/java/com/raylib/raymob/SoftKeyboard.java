/*
 *  raymob License (MIT)
 *
 *  Copyright (c) 2023-2024 Le Juez Victor
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */

package com.raylib.raymob;

import android.view.inputmethod.InputMethodManager;
import android.content.Context;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.KeyEvent;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;

import java.util.ArrayDeque;

public class SoftKeyboard {

    private static final int SYNTHETIC_KEY_CODE = 1;

    private final InputMethodManager imm;
    private final EditText inputView;
    private final ArrayDeque<PendingKey> pendingKeys = new ArrayDeque<>();
    private String previousText = "";
    private boolean suppressTextWatcher = false;
    private KeyEvent lastKeyEvent = null;

    private static final class PendingKey {
        final int keyCode;
        final char label;
        final int unicode;

        PendingKey(int keyCode, char label, int unicode) {
            this.keyCode = keyCode;
            this.label = label;
            this.unicode = unicode;
        }
    }

    public SoftKeyboard(Context context, EditText inputView) {
        imm = (InputMethodManager)context.getSystemService(Context.INPUT_METHOD_SERVICE);
        this.inputView = inputView;
        this.inputView.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {}

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {}

            @Override
            public void afterTextChanged(Editable editable) {
                synchronized (SoftKeyboard.this) {
                    if (suppressTextWatcher) return;
                    queueTextDelta(editable.toString());
                }
            }
        });
        this.inputView.setOnEditorActionListener((textView, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_DONE || (event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER)) {
                synchronized (SoftKeyboard.this) {
                    pendingKeys.addLast(new PendingKey(KeyEvent.KEYCODE_ENTER, '\n', '\n'));
                }
                return true;
            }
            return false;
        });
    }

    /* PUBLIC FOR JNI (raymob.h) */

    public void showKeyboard() {
        if (imm == null || inputView == null) return;

        inputView.post(() -> {
            inputView.requestFocus();
            imm.showSoftInput(inputView, InputMethodManager.SHOW_FORCED);
        });
    }

    public void hideKeyboard() {
        if (imm == null || inputView == null) return;

        inputView.post(() -> {
            imm.hideSoftInputFromWindow(inputView.getWindowToken(), 0);
            inputView.clearFocus();
        });
    }

    public void setKeyboardText(String text) {
        if (inputView == null) return;

        final String value = text == null ? "" : text;
        synchronized (this) {
            suppressTextWatcher = true;
            previousText = value;
            pendingKeys.clear();
            lastKeyEvent = null;
        }
        inputView.post(() -> {
            synchronized (SoftKeyboard.this) {
                try {
                    inputView.setText(previousText);
                    inputView.setSelection(previousText.length());
                } finally {
                    suppressTextWatcher = false;
                }
            }
        });
    }

    public int getLastKeyCode() {
        synchronized (this) {
            PendingKey pending = pendingKeys.peekFirst();
            if (pending != null) return pending.keyCode;
            if (lastKeyEvent != null) return lastKeyEvent.getKeyCode();
            return 0;
        }
    }

    public char getLastKeyLabel() {
        synchronized (this) {
            PendingKey pending = pendingKeys.peekFirst();
            if (pending != null) return pending.label;
            if (lastKeyEvent != null) return lastKeyEvent.getDisplayLabel();
            return '\0';
        }
    }

    public int getLastKeyUnicode() {
        synchronized (this) {
            PendingKey pending = pendingKeys.peekFirst();
            if (pending != null) return pending.unicode;
            if (lastKeyEvent != null) return lastKeyEvent.getUnicodeChar();
            return 0;
        }
    }

    public void clearLastKeyEvent() {
        synchronized (this) {
            lastKeyEvent = null;
            if (!pendingKeys.isEmpty()) {
                pendingKeys.removeFirst();
            }
        }
    }

    /* PRIVATE FOR JNI (raymob.h) */

    public void onKeyUpEvent(KeyEvent event) {
        synchronized (this) {
            lastKeyEvent = event;
        }
    }

    private void queueTextDelta(String currentText) {
        int prefix = 0;
        int previousLength = previousText.length();
        int currentLength = currentText.length();

        while (prefix < previousLength && prefix < currentLength && previousText.charAt(prefix) == currentText.charAt(prefix)) {
            prefix++;
        }

        int suffix = 0;
        while (suffix < previousLength - prefix
                && suffix < currentLength - prefix
                && previousText.charAt(previousLength - 1 - suffix) == currentText.charAt(currentLength - 1 - suffix)) {
            suffix++;
        }

        int removed = previousLength - prefix - suffix;
        for (int i = 0; i < removed; i++) {
            pendingKeys.addLast(new PendingKey(KeyEvent.KEYCODE_DEL, '\b', '\b'));
        }

        int insertedEnd = currentLength - suffix;
        for (int i = prefix; i < insertedEnd; i++) {
            char c = currentText.charAt(i);
            int keyCode = (c == '\n') ? KeyEvent.KEYCODE_ENTER : SYNTHETIC_KEY_CODE;
            pendingKeys.addLast(new PendingKey(keyCode, c, c));
        }

        previousText = currentText;
    }
}
