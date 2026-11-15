package tinymq.jms;

import jakarta.jms.MessageNotWriteableException;
import jakarta.jms.TextMessage;

public final class TinyTextMessage extends TinyMessage implements TextMessage {
    private String text;

    public TinyTextMessage() {}

    public TinyTextMessage(String text) { this.text = text; }

    @Override
    public void setText(String value) throws MessageNotWriteableException {
        requireBodyWriteable();
        text = value;
    }

    @Override
    public String getText() { return text; }

    @Override
    Object snapshotBody() { return text; }

    @Override
    void discardBody() { text = null; }
}
