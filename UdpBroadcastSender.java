import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.util.Scanner;

public class UdpBroadcastSender {

    public static void main(String[] args) {
        Scanner scanner = new Scanner(System.in);
        DatagramSocket socket = null;

        try {
            // Dirección de broadcast y puerto
            InetAddress broadcastAddress = InetAddress.getByName("255.255.255.255");
            int port = 9999;

            // Crear socket UDP y permitir broadcast
            socket = new DatagramSocket();
            socket.setBroadcast(true);

            System.out.println("Escribe un mensaje ('EXIT' para salir):");

            while (true) {
                System.out.print("> ");
                String message = scanner.nextLine();

                if (message.equalsIgnoreCase("EXIT")) {
                    System.out.println("[+] Cerrando programa...");
                    break;
                }

                byte[] buffer = message.getBytes();
                DatagramPacket packet = new DatagramPacket(buffer, buffer.length, broadcastAddress, port);

                socket.send(packet);
                System.out.println("[+] Mensaje enviado por broadcast a " + broadcastAddress.getHostAddress() + ":" + port);

                // Esperar 3 segundos antes del siguiente mensaje
                Thread.sleep(3000);
            }

        } catch (Exception e) {
            System.err.println("[-] Error al enviar el paquete UDP: " + e.getMessage());
        } finally {
            if (socket != null && !socket.isClosed()) {
                socket.close();
                System.out.println("[+] Socket cerrado correctamente.");
            }
            scanner.close();
        }
    }
}