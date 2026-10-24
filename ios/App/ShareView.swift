import Combine
import SwiftUI
import UIKit

/// Settings for sharing (stored where the broadcast extension can read them) and the
/// state of a running share: room code, link, viewers.
struct ShareView: View {
    @AppStorage(Shared.Key.server, store: Shared.defaults) private var server = Shared.defaultServer
    @AppStorage(Shared.Key.password, store: Shared.defaults) private var password = ""
    @AppStorage(Shared.Key.shareAudio, store: Shared.defaults) private var shareAudio = true
    @State private var status = ShareStatus.load()
    @State private var copied = false
    private let refresh = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    private var running: Bool {
        // a crashed or killed extension leaves its last state behind: trust it for a while only
        [.connecting, .live, .paused].contains(status.state) && Date().timeIntervalSince(status.updated) < 6 * 3600
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Szerver, pl. mozizas.hu", text: $server)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    SecureField("Megosztási jelszó", text: $password)
                    Toggle("Az alkalmazások hangja is", isOn: $shareAudio)
                } header: {
                    Text("Beállítások")
                } footer: {
                    if Shared.normalizedServer(server) == nil { Text("Érvénytelen szervercím.").foregroundStyle(.red) }
                }

                Section {
                    Button {
                        BroadcastPicker.shared.show()
                    } label: {
                        Label(running ? "Megosztás leállítása / váltás" : "Képernyő megosztása", systemImage: running ? "stop.circle.fill" : "record.circle")
                            .font(.headline)
                            .frame(maxWidth: .infinity, minHeight: 44)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(password.isEmpty || Shared.normalizedServer(server) == nil)
                    .listRowBackground(Color.clear)
                    .background(BroadcastPickerHost().frame(width: 1, height: 1).opacity(0.01))
                } footer: {
                    Text("Az iOS megkérdezi: „Közvetítés indítása”. Leállítani a képernyő tetején lévő piros jelzéssel vagy a Vezérlőközpontból is lehet. A megosztás akkor is megy, ha közben más alkalmazásra váltasz.")
                }

                Section("Állapot") {
                    statusRows
                }
            }
            .navigationTitle("Screen Share")
        }
        .onReceive(refresh) { _ in
            let latest = ShareStatus.load()
            if latest != status { status = latest }
        }
    }

    @ViewBuilder private var statusRows: some View {
        switch status.state {
        case .idle:
            Text("Nincs megosztás.").foregroundStyle(.secondary)
        case .failed:
            Text(status.message ?? "A megosztás leállt.").foregroundStyle(.red)
        case .connecting:
            HStack { ProgressView(); Text(status.message ?? "Csatlakozás a szerverhez…") }
        case .live, .paused:
            if status.state == .paused { Text("Szünetel").foregroundStyle(.orange) }
            if let code = status.code {
                LabeledContent("Szobakód", value: code).font(.title3.monospaced())
            }
            LabeledContent("Nézők", value: "\(status.viewers)")
            if let link = status.link, let url = URL(string: link) {
                Text(link).font(.footnote.monospaced()).textSelection(.enabled)
                Button(copied ? "Másolva ✓" : "Link másolása") {
                    UIPasteboard.general.string = link
                    copied = true
                    DispatchQueue.main.asyncAfter(deadline: .now() + 2) { copied = false }
                }
                ShareLink(item: url) { Label("Link küldése…", systemImage: "square.and.arrow.up") }
            }
        }
    }
}
