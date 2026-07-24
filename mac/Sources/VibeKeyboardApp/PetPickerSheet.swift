import SwiftUI
import VibeBoardKit

/// Independent picker sheet for browsing and previewing pets.
///
/// Replaces the inline `Picker` that rendered every catalog item at once
/// (the primary source of jank once Petdex returned a large list). Uses a
/// native `List` for row virtualization and a local search state so typing
/// never triggers a full `ScreenPage` re-evaluation.
struct PetPickerSheet: View {
    @ObservedObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var search: String

    init(model: AppModel) {
        self.model = model
        // Seed from any previously typed query so re-opening keeps context.
        _search = State(initialValue: model.petSearch)
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            searchBar
            Divider().padding(.top, 8)
            content
        }
        .frame(minWidth: 640, minHeight: 440)
        .onAppear {
            if let pet = model.selectedPet {
                model.loadPetPreview(for: pet)
            }
        }
    }

    // MARK: - Header

    private var header: some View {
        HStack {
            Text("Choose a pet").font(.headline)
            Spacer()
            if model.petPreviewLoading {
                ProgressView().scaleEffect(0.7)
            }
            Button("Done") { dismiss() }
                .keyboardShortcut(.return)
        }
        .padding()
    }

    // MARK: - Search

    private var searchBar: some View {
        HStack {
            Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
            TextField("Search by name or slug", text: $search)
                .textFieldStyle(.roundedBorder)
                .onChange(of: search) { model.petSearch = $0 }
            if !search.isEmpty {
                Button {
                    search = ""
                    model.petSearch = ""
                } label: {
                    Image(systemName: "xmark.circle.fill")
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
            }
            Text("\(filteredPets.count) / \(model.pets.count)")
                .font(.caption)
                .foregroundStyle(.secondary)
                .monospacedDigit()
        }
        .padding(.horizontal)
    }

    // MARK: - Content (list + preview)

    private var content: some View {
        HStack(spacing: 0) {
            listSection
            Divider()
            previewSection
                .frame(width: 280)
        }
    }

    private var listSection: some View {
        Group {
            if filteredPets.isEmpty {
                EmptyCatalogState(catalogEmpty: model.pets.isEmpty)
            } else {
                List(filteredPets) { pet in
                    PetRow(
                        pet: pet,
                        isSelected: model.selectedPetID == pet.id,
                        isUploaded: model.uploadedPetID == pet.id
                    )
                    .contentShape(Rectangle())
                    .onTapGesture {
                        model.selectedPetID = pet.id
                        model.loadPetPreview(for: pet)
                    }
                }
                .listStyle(.inset)
            }
        }
    }

    private var previewSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Preview").font(.headline)
            ZStack {
                Color.black
                if !model.petPreviewFrames.isEmpty {
                    PetPreviewAnimationView(
                        frames: model.petPreviewFrames,
                        durationsMS: model.petPreviewFrameDurationsMS
                    )
                    .id(model.petPreviewItemID)
                    .padding(8)
                } else if model.petPreviewLoading {
                    ProgressView().tint(.white)
                } else if model.petPreviewFailed {
                    VStack(spacing: 6) {
                        Image(systemName: "exclamationmark.triangle")
                            .foregroundStyle(.orange)
                        Text("Preview failed")
                            .font(.caption)
                            .foregroundStyle(.white.opacity(0.7))
                    }
                } else {
                    Text("Select a pet")
                        .foregroundStyle(.white.opacity(0.6))
                        .font(.callout)
                }
            }
            .aspectRatio(119.0 / 129.0, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 10))

            if let pet = model.selectedPet {
                Text(pet.displayName).font(.callout.weight(.semibold))
                Text(pet.attribution)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                Text("\(model.petAnimationChoice.label) · \(model.petPreviewFrames.count) frames")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            } else {
                Text("No pet selected")
                    .foregroundStyle(.secondary)
                    .font(.callout)
            }

            Spacer()
        }
        .padding()
    }

    // MARK: - Filtering

    private var filteredPets: [PetCatalogItem] {
        let query = search.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !query.isEmpty else { return model.pets }
        return model.pets.filter {
            $0.displayName.localizedCaseInsensitiveContains(query)
                || $0.slug.localizedCaseInsensitiveContains(query)
        }
    }
}

private struct PetRow: View {
    let pet: PetCatalogItem
    let isSelected: Bool
    let isUploaded: Bool

    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(pet.displayName)
                    if isUploaded {
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundStyle(.green)
                            .font(.caption)
                    }
                }
                Text(pet.attribution)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Text(pet.kind)
                .font(.caption2)
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .background(.quaternary, in: Capsule())
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 3)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(isSelected ? Color.accentColor.opacity(0.15) : Color.clear)
        )
        .overlay(alignment: .leading) {
            if isSelected {
                Rectangle()
                    .fill(Color.accentColor)
                    .frame(width: 3)
            }
        }
    }
}

private struct EmptyCatalogState: View {
    let catalogEmpty: Bool

    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: catalogEmpty ? "pawprint" : "magnifyingglass")
                .font(.title2)
                .foregroundStyle(.secondary)
            Text(catalogEmpty ? "No pets yet" : "No matches")
                .font(.headline)
            Text(catalogEmpty
                ? "Tap Refresh to load the Petdex catalog."
                : "Try a different search term.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

/// Plays a decoded pet spritesheet as a looping animation, honoring per-frame
/// durations. Driven by `TimelineView` (declarative, main-actor safe) and
/// re-created via `.id` whenever a new pet is selected so the timeline
/// restarts from frame zero.
struct PetPreviewAnimationView: View {
    let frames: [CGImage]
    let durationsMS: [Int]
    @State private var startDate = Date()

    private var totalMS: Int { max(1, durationsMS.reduce(0, +)) }

    var body: some View {
        TimelineView(.periodic(from: startDate, by: 1.0 / 30.0)) { context in
            let elapsed = Int(context.date.timeIntervalSince(startDate) * 1000) % totalMS
            let idx = frameIndex(at: elapsed)
            Image(decorative: frames[idx], scale: 1)
                .resizable()
                .interpolation(.none)
                .scaledToFit()
        }
    }

    private func frameIndex(at elapsedMS: Int) -> Int {
        var remaining = elapsedMS
        for (i, duration) in durationsMS.enumerated() {
            if remaining < duration { return i }
            remaining -= duration
        }
        return max(0, frames.count - 1)
    }
}
