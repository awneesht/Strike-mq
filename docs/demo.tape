Output docs/demo.gif

Set Shell "bash"
Set FontSize 14
Set Width 1000
Set Height 600
Set Theme "Catppuccin Mocha"
Set Padding 20
Set TypingSpeed 40ms

# Start the broker
Type "./build/strikemq &"
Enter
Sleep 3s

# Produce messages with kcat
Type "echo -e 'hello\nworld\nstrike' | kcat -b 127.0.0.1:9092 -P -t my-topic"
Enter
Sleep 2s

# Consume messages
Type "kcat -b 127.0.0.1:9092 -C -t my-topic -e"
Enter
Sleep 3s

# Peek via REST API
Type "curl -s localhost:8080/v1/topics/my-topic/messages?limit=3 | python3 -m json.tool"
Enter
Sleep 4s

# Stop the broker
Type "kill %1 2>/dev/null"
Enter
Sleep 1s
